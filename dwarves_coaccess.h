#ifndef _DWARVES_COACCESS_H_
#define _DWARVES_COACCESS_H_ 1
/*
  SPDX-License-Identifier: GPL-2.0-only

  Co-access profile loader and cache-line-aware struct reordering for pahole.
  Consumes edge lists emitted by kstruct-tuner (fieldA fieldB weight).
*/

#include <stddef.h>
#include <stdio.h>

struct class;
struct cu;
struct coaccess_profile;

/* 0 = auto: expand top pairs until enough scalar hot fields are covered. */
#define COACCESS_TOP_PAIRS_AUTO 0

struct coaccess_profile *coaccess__load(const char *path);
void coaccess__delete(struct coaccess_profile *prof);

void class__reorganize_coaccess(struct class *cls, const struct cu *cu,
				const struct coaccess_profile *prof,
				size_t cacheline_bytes, unsigned top_pairs,
				int verbose, FILE *fp);

#endif /* _DWARVES_COACCESS_H_ */
