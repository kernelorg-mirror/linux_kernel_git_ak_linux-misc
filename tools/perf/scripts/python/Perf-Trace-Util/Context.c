// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Context.c.  Python interfaces for perf script.
 *
 * Copyright (C) 2010 Tom Zanussi <tzanussi@gmail.com>
 */

/*
 * Use Py_ssize_t for '#' formats to avoid DeprecationWarning: PY_SSIZE_T_CLEAN
 * will be required for '#' formats.
 */
#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include "../../../util/trace-event.h"
#include "../../../util/event.h"
#include "../../../util/symbol.h"
#include "../../../util/thread.h"
#include "../../../util/map.h"
#include "../../../util/maps.h"
#include "../../../util/auxtrace.h"
#include "../../../util/session.h"
#include "../../../util/srcline.h"
#include "../../../util/srccode.h"
#include "../../../util/dso.h"

#if PY_MAJOR_VERSION < 3
#define _PyCapsule_GetPointer(arg1, arg2) \
  PyCObject_AsVoidPtr(arg1)
#define _PyBytes_FromStringAndSize(arg1, arg2) \
  PyString_FromStringAndSize((arg1), (arg2))
#define _PyUnicode_AsUTF8(arg) \
  PyString_AsString(arg)

PyMODINIT_FUNC initperf_trace_context(void);
#else
#define _PyCapsule_GetPointer(arg1, arg2) \
  PyCapsule_GetPointer((arg1), (arg2))
#define _PyBytes_FromStringAndSize(arg1, arg2) \
  PyBytes_FromStringAndSize((arg1), (arg2))
#define _PyUnicode_AsUTF8(arg) \
  PyUnicode_AsUTF8(arg)

PyMODINIT_FUNC PyInit_perf_trace_context(void);
#endif

static struct scripting_context *get_args(PyObject *args, const char *name, PyObject **arg2)
{
	int cnt = 1 + !!arg2;
	PyObject *context;

	if (!PyArg_UnpackTuple(args, name, 1, cnt, &context, arg2))
		return NULL;

	return _PyCapsule_GetPointer(context, NULL);
}

static struct scripting_context *get_scripting_context(PyObject *args)
{
	return get_args(args, "context", NULL);
}

#ifdef HAVE_LIBTRACEEVENT
static PyObject *perf_trace_context_common_pc(PyObject *obj, PyObject *args)
{
	struct scripting_context *c = get_scripting_context(args);

	if (!c)
		return NULL;

	return Py_BuildValue("i", common_pc(c));
}

static PyObject *perf_trace_context_common_flags(PyObject *obj,
						 PyObject *args)
{
	struct scripting_context *c = get_scripting_context(args);

	if (!c)
		return NULL;

	return Py_BuildValue("i", common_flags(c));
}

static PyObject *perf_trace_context_common_lock_depth(PyObject *obj,
						      PyObject *args)
{
	struct scripting_context *c = get_scripting_context(args);

	if (!c)
		return NULL;

	return Py_BuildValue("i", common_lock_depth(c));
}
#endif

static PyObject *perf_sample_insn(PyObject *obj, PyObject *args)
{
	struct scripting_context *c = get_scripting_context(args);

	if (!c)
		return NULL;

	if (c->sample->ip && !c->sample->insn_len && thread__maps(c->al->thread)) {
		struct machine *machine =  maps__machine(thread__maps(c->al->thread));

		script_fetch_insn(c->sample, c->al->thread, machine);
	}
	if (!c->sample->insn_len)
		Py_RETURN_NONE; /* N.B. This is a return statement */

	return _PyBytes_FromStringAndSize(c->sample->insn, c->sample->insn_len);
}

static PyObject *perf_set_itrace_options(PyObject *obj, PyObject *args)
{
	struct scripting_context *c;
	const char *itrace_options;
	int retval = -1;
	PyObject *str;

	c = get_args(args, "itrace_options", &str);
	if (!c)
		return NULL;

	if (!c->session || !c->session->itrace_synth_opts)
		goto out;

	if (c->session->itrace_synth_opts->set) {
		retval = 1;
		goto out;
	}

	itrace_options = _PyUnicode_AsUTF8(str);

	retval = itrace_do_parse_synth_opts(c->session->itrace_synth_opts, itrace_options, 0);
out:
	return Py_BuildValue("i", retval);
}

static PyObject *perf_sample_src(PyObject *obj, PyObject *args, bool get_srccode)
{
	struct scripting_context *c = get_scripting_context(args);
	unsigned int line = 0, disc = 0;
	char *srcfile = NULL;
	char *srccode = NULL;
	PyObject *result;
	struct map *map;
	struct dso *dso;
	int len = 0;
	u64 addr;

	if (!c)
		return NULL;

	map = c->al->map;
	addr = c->al->addr;
	dso = map ? map__dso(map) : NULL;

	if (dso)
		srcfile = get_srcline_split(dso, map__rip_2objdump(map, addr), &line, &disc,
				NULL);

	if (get_srccode) {
		if (srcfile)
			srccode = find_sourceline(srcfile, line, &len);
		result = Py_BuildValue("(sIs#)", srcfile, line, srccode, (Py_ssize_t)len);
	} else {
		result = Py_BuildValue("(sI)", srcfile, line);
	}

	free(srcfile);

	return result;
}

static PyObject *perf_sample_srcline(PyObject *obj, PyObject *args)
{
	return perf_sample_src(obj, args, false);
}

static PyObject *perf_sample_srccode(PyObject *obj, PyObject *args)
{
	return perf_sample_src(obj, args, true);
}

static PyObject *do_resolve_ip(struct scripting_context *c, PyObject *ipp)
{
	struct addr_location al;
	struct dso *dso;
	u8 cpumode;
	char *srcfile = NULL;
	PyObject *result = NULL;
	bool kernel;
	u64 ip = PyLong_AsUnsignedLongLong(ipp);
	unsigned line = 0, disc = 0;
	struct inline_node *inode = calloc(sizeof(struct inline_node), 1);

	if (!inode)
		goto err2;

	INIT_LIST_HEAD(&inode->val);
	kernel = machine__kernel_ip(c->machine, ip);
	if (kernel)
		cpumode = PERF_RECORD_MISC_KERNEL;
	else
		cpumode = PERF_RECORD_MISC_USER;

	addr_location__init(&al);
	if (machine__resolve(c->machine, &al, c->sample) < 0)
		goto err2;
	if (!thread__find_map(al.thread, cpumode, ip, &al))
		goto err;
	if (!al.map)
		goto err;
	dso = map__dso(al.map);
	if (!dso)
		goto err;
	if (dso__data(dso)->status == DSO_DATA_STATUS_ERROR)
		goto err;
	srcfile = get_srcline_split(dso, map__rip_2objdump(al.map, al.addr), &line,
				    &disc, inode);
	if (srcfile && srcfile != SRCLINE_UNKNOWN) {
		if (!list_empty(&inode->val)) {
			PyObject *inlines;
			struct inline_list *ilist;
			int num;
		       	num = 0;
			list_for_each_entry (ilist, &inode->val, list)
				num++;
			inlines = PyTuple_New(num);
			if (!inlines)
				goto out;
			num = 0;
			list_for_each_entry (ilist, &inode->val, list) {
				PyTuple_SetItem(inlines, num,
						Py_BuildValue("(ssII)",
							ilist->srcline, ilist->symbol->name,
							ilist->line, ilist->disc));
				num++;
			}
			result = Py_BuildValue("(sIIO)", srcfile, line, disc, inlines);
			Py_DECREF(inlines);
		out:
		} else {
			result = Py_BuildValue("(sII)", srcfile, line, disc);
		}
	}
	zfree_srcline(&srcfile);
err:
	addr_location__exit(&al);
err2:
	if (!result)
		return Py_BuildValue("");
	inline_node__delete(inode);
	return result;
}

/* Resolve srcfile/line/disc for brstack from/to. */

static PyObject *perf_brstack_srcline(PyObject *obj, PyObject *args)
{
	PyObject *brstack = NULL;
	PyObject *from, *to;
	struct scripting_context *c = get_args(args, "brstack", &brstack);
	if (!c)
		return NULL;
	from = do_resolve_ip(c, PyDict_GetItemString(brstack, "from"));
	to = do_resolve_ip(c, PyDict_GetItemString(brstack, "to"));
	return Py_BuildValue("(OO)", from, to);
}

/* Resolve an numerical address to srcfile/line/disc */

static PyObject *perf_resolve_ip(PyObject *obj, PyObject *args)
{
	PyObject *ip;
	struct scripting_context *c = get_args(args, "ip", &ip);
	if (!c)
		return NULL;
	return do_resolve_ip(c, ip);
}

static PyMethodDef ContextMethods[] = {
#ifdef HAVE_LIBTRACEEVENT
	{ "common_pc", perf_trace_context_common_pc, METH_VARARGS,
	  "Get the common preempt count event field value."},
	{ "common_flags", perf_trace_context_common_flags, METH_VARARGS,
	  "Get the common flags event field value."},
	{ "common_lock_depth", perf_trace_context_common_lock_depth,
	  METH_VARARGS,	"Get the common lock depth event field value."},
#endif
	{ "perf_sample_insn", perf_sample_insn,
	  METH_VARARGS,	"Get the machine code instruction."},
	{ "perf_set_itrace_options", perf_set_itrace_options,
	  METH_VARARGS,	"Set --itrace options."},
	{ "perf_sample_srcline", perf_sample_srcline,
	  METH_VARARGS,	"Get source file name and line number."},
	{ "perf_sample_srccode", perf_sample_srccode,
	  METH_VARARGS,	"Get source file name, line number and line."},
	{ "perf_brstack_srcline", perf_brstack_srcline,
	  METH_VARARGS, "Get source file name, line number, discriminator, inline stack for from/to of a brstack entry." },
	{ "perf_resolve_ip", perf_resolve_ip,
	  METH_VARARGS, "Get source file name, line number, discriminator, inline stack for numerical IP." },
	{ NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION < 3
PyMODINIT_FUNC initperf_trace_context(void)
{
	(void) Py_InitModule("perf_trace_context", ContextMethods);
}
#else
PyMODINIT_FUNC PyInit_perf_trace_context(void)
{
	static struct PyModuleDef moduledef = {
		PyModuleDef_HEAD_INIT,
		"perf_trace_context",	/* m_name */
		"",			/* m_doc */
		-1,			/* m_size */
		ContextMethods,		/* m_methods */
		NULL,			/* m_reload */
		NULL,			/* m_traverse */
		NULL,			/* m_clear */
		NULL,			/* m_free */
	};
	PyObject *mod;

	mod = PyModule_Create(&moduledef);
	/* Add perf_script_context to the module so it can be imported */
	PyObject_SetAttrString(mod, "perf_script_context", Py_None);

	return mod;
}
#endif
