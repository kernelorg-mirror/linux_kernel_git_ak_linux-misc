/*
 * intel_pt_log.h: Intel Processor Trace support
 * Copyright (c) 2013-2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#ifndef INCLUDE__INTEL_PT_LOG_H__
#define INCLUDE__INTEL_PT_LOG_H__

#include <stdint.h>
#include <inttypes.h>

struct intel_pt_pkt;

void intel_pt_log_set_name(const char *name);

extern int intel_pt_no_logging;

void __intel_pt_log_packet(const struct intel_pt_pkt *packet, int pkt_len,
			 uint64_t pos, const unsigned char *buf);

static inline void intel_pt_log_packet(const struct intel_pt_pkt *packet,
				       int pkt_len,
				       uint64_t pos, const unsigned char *buf)
{
	if (!intel_pt_no_logging)
		__intel_pt_log_packet(packet, pkt_len, pos, buf);
}

struct intel_pt_insn;

void __intel_pt_log_insn(struct intel_pt_insn *intel_pt_insn, uint64_t ip);

static inline void intel_pt_log_insn(struct intel_pt_insn *intel_pt_insn,
				     uint64_t ip)
{
	if (!intel_pt_no_logging)
		__intel_pt_log_insn(intel_pt_insn, ip);
}

__attribute__((format(printf, 1, 2)))
void intel_pt_log(const char *fmt, ...);

#define x64_fmt "0x%" PRIx64

static inline void intel_pt_log_at(const char *msg, uint64_t u)
{
	intel_pt_log("%s at " x64_fmt "\n", msg, u);
}

static inline void intel_pt_log_to(const char *msg, uint64_t u)
{
	intel_pt_log("%s to " x64_fmt "\n", msg, u);
}

#endif
