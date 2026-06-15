/*
  SPDX-License-Identifier: GPL-2.0-only

  Trace-driven struct layout: reorder members by a co-access affinity profile.
  Sibling to class__reorganize() (hole packing); pulls the hottest co-access
  cluster onto cache line 0 using greedy affinity clustering capped at one line.
*/

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dwarves.h"
#include "dwarves_coaccess.h"
#include "dwarves_reorganize.h"
#include "dutil.h"

struct coaccess_edge {
	char *field_a;
	char *field_b;
	int weight;
};

struct coaccess_profile {
	struct coaccess_edge *edges;
	size_t nr_edges;
};

#define MAX_FIELDS      512
#define MAX_ALIASES     8
#define AUTO_MIN_SCALAR 6
#define AUTO_MAX_EDGES  128

struct field_info {
	struct class_member *member;
	char name[128];
	char aliases[MAX_ALIASES][128];
	int nr_aliases;
	uint16_t size;
	int cluster;
};

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}

static int coaccess__edge_cmp(const void *va, const void *vb)
{
	const struct coaccess_edge *a = va, *b = vb;

	return b->weight - a->weight;
}

struct coaccess_profile *coaccess__load(const char *path)
{
	FILE *fp = fopen(path, "r");
	struct coaccess_profile *prof;
	char line[512];
	size_t cap = 64, nr = 0;

	if (!fp) {
		fprintf(stderr, "coaccess: cannot open '%s': %s\n",
			path, strerror(errno));
		return NULL;
	}

	prof = zalloc(sizeof(*prof));
	if (!prof) {
		fclose(fp);
		return NULL;
	}

	prof->edges = malloc(cap * sizeof(*prof->edges));
	if (!prof->edges) {
		free(prof);
		fclose(fp);
		return NULL;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *a, *b, *wstr, *save = NULL;
		int w;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		a = strtok_r(line, " \t\n", &save);
		b = strtok_r(NULL, " \t\n", &save);
		wstr = strtok_r(NULL, " \t\n", &save);
		if (!a || !b || !wstr)
			continue;
		w = atoi(wstr);
		if (w <= 0)
			continue;
		if (nr >= cap) {
			cap *= 2;
			prof->edges = realloc(prof->edges, cap * sizeof(*prof->edges));
			if (!prof->edges)
				goto enomem;
		}
		prof->edges[nr].field_a = xstrdup(a);
		prof->edges[nr].field_b = xstrdup(b);
		prof->edges[nr].weight = w;
		if (!prof->edges[nr].field_a || !prof->edges[nr].field_b)
			goto enomem;
		nr++;
	}

	fclose(fp);
	prof->nr_edges = nr;
	if (nr > 1)
		qsort(prof->edges, nr, sizeof(*prof->edges), coaccess__edge_cmp);
	return prof;

enomem:
	fclose(fp);
	coaccess__delete(prof);
	return NULL;
}

void coaccess__delete(struct coaccess_profile *prof)
{
	size_t i;

	if (!prof)
		return;
	for (i = 0; i < prof->nr_edges; i++) {
		free(prof->edges[i].field_a);
		free(prof->edges[i].field_b);
	}
	free(prof->edges);
	free(prof);
}

static int member__size(const struct class_member *m)
{
	if (m->bitfield_size != 0)
		return 0;
	return m->byte_size ? m->byte_size : 1;
}

static void collect_nested_aliases(const struct cu *cu, struct class_member *m,
				   struct field_info *fi)
{
	struct tag *t = cu__type(cu, m->tag.type);
	struct class_member *nm;

	if (!t || (!tag__is_struct(t) && !tag__is_union(t)))
		return;

	type__for_each_data_member(tag__type(t), nm) {
		const char *nn = class_member__name(nm);

		if (!nn || fi->nr_aliases >= MAX_ALIASES)
			continue;
		strncpy(fi->aliases[fi->nr_aliases], nn, 127);
		fi->aliases[fi->nr_aliases][127] = '\0';
		fi->nr_aliases++;
	}
}

static int field_name_matches(const struct field_info *fi, const char *name)
{
	int i;

	if (fi->name[0] && strcmp(fi->name, name) == 0)
		return 1;
	for (i = 0; i < fi->nr_aliases; i++) {
		if (strcmp(fi->aliases[i], name) == 0)
			return 1;
	}
	return 0;
}

static int find_field_index(struct field_info *fields, int nr, const char *name)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (field_name_matches(&fields[i], name))
			return i;
	}
	return -1;
}

static int collect_fields(struct class *cls, const struct cu *cu,
			  struct field_info *fields)
{
	struct class_member *m;
	int nr = 0;

	type__for_each_data_member(&cls->type, m) {
		const char *name = class_member__name(m);

		if (m->bitfield_size != 0)
			continue;
		if (nr >= MAX_FIELDS)
			break;
		fields[nr].member = m;
		if (name)
			strncpy(fields[nr].name, name, sizeof(fields[nr].name) - 1);
		else
			fields[nr].name[0] = '\0';
		fields[nr].name[sizeof(fields[nr].name) - 1] = '\0';
		fields[nr].nr_aliases = 0;
		collect_nested_aliases(cu, m, &fields[nr]);
		fields[nr].size = member__size(m);
		fields[nr].cluster = nr;
		nr++;
	}
	return nr;
}

static int hot_name_resolves(const struct field_info *fields, int nr,
			     const char *name, size_t cacheline_bytes)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (field_name_matches(&fields[i], name) &&
		    fields[i].size <= cacheline_bytes)
			return 1;
	}
	return 0;
}

static unsigned effective_top_pairs(const struct coaccess_profile *prof,
				    unsigned top_pairs, struct field_info *fields,
				    int nr, size_t cacheline_bytes)
{
	char seen[MAX_FIELDS][128];
	size_t i, limit;
	int nr_seen = 0;

	if (top_pairs != COACCESS_TOP_PAIRS_AUTO)
		return top_pairs;

	/* Expand the window until enough DISTINCT scalar fields are covered.
	   Counting occurrences would stop early when a few fields repeat across
	   the strongest edges, leaving co-accessed siblings behind. */
	limit = prof->nr_edges < AUTO_MAX_EDGES ? prof->nr_edges : AUTO_MAX_EDGES;
	for (i = 0; i < limit; i++) {
		const struct coaccess_edge *e = &prof->edges[i];
		int j;

		for (j = 0; j < 2; j++) {
			const char *f = j ? e->field_b : e->field_a;
			int k, dup = 0;

			if (!hot_name_resolves(fields, nr, f, cacheline_bytes))
				continue;
			for (k = 0; k < nr_seen; k++) {
				if (strcmp(seen[k], f) == 0) {
					dup = 1;
					break;
				}
			}
			if (dup || nr_seen >= MAX_FIELDS)
				continue;
			strncpy(seen[nr_seen], f, sizeof(seen[0]) - 1);
			seen[nr_seen][sizeof(seen[0]) - 1] = '\0';
			nr_seen++;
		}
		if (nr_seen >= AUTO_MIN_SCALAR)
			return (unsigned)(i + 1);
	}
	return (unsigned)limit;
}

static void mark_hot_fields(const struct coaccess_profile *prof,
			    unsigned top_pairs, struct field_info *fields, int nr,
			    size_t cacheline_bytes, char hot[][128], int *nr_hot)
{
	size_t i;
	unsigned use_pairs = effective_top_pairs(prof, top_pairs, fields, nr,
						 cacheline_bytes);

	*nr_hot = 0;
	for (i = 0; i < prof->nr_edges && i < use_pairs; i++) {
		const struct coaccess_edge *e = &prof->edges[i];
		int j;

		for (j = 0; j < 2; j++) {
			const char *f = j ? e->field_b : e->field_a;
			int k, dup = 0;

			if (!hot_name_resolves(fields, nr, f, cacheline_bytes))
				continue;
			for (k = 0; k < *nr_hot; k++) {
				if (strcmp(hot[k], f) == 0) {
				 dup = 1;
				 break;
				}
			}
			if (!dup && *nr_hot < MAX_FIELDS) {
				strncpy(hot[*nr_hot], f, 127);
				hot[*nr_hot][127] = '\0';
				(*nr_hot)++;
			}
		}
	}
}

static int cluster_bytes(struct field_info *fields, int nr, int cid)
{
	int sum = 0, i;

	for (i = 0; i < nr; i++) {
		if (fields[i].cluster == cid)
			sum += fields[i].size;
	}
	return sum;
}

/* Total co-access weight contained entirely within cluster `cid`. Used to put
   the hottest cluster on line 0 regardless of source declaration order. */
static long long cluster_weight(struct field_info *fields, int nr,
				const struct coaccess_profile *prof, int cid)
{
	long long sum = 0;
	size_t ei;

	for (ei = 0; ei < prof->nr_edges; ei++) {
		const struct coaccess_edge *e = &prof->edges[ei];
		int ia = find_field_index(fields, nr, e->field_a);
		int ib = find_field_index(fields, nr, e->field_b);

		if (ia < 0 || ib < 0)
			continue;
		if (fields[ia].cluster == cid && fields[ib].cluster == cid)
			sum += e->weight;
	}
	return sum;
}

static int field_is_hot(const char hot[][128], int nr_hot,
			const struct field_info *fi)
{
	int i;

	for (i = 0; i < nr_hot; i++) {
		if (field_name_matches(fi, hot[i]))
			return 1;
	}
	return 0;
}

static void greedy_affinity_clusters(struct field_info *fields, int nr,
				     const struct coaccess_profile *prof,
				     const char hot[][128], int nr_hot,
				     size_t max_bytes)
{
	size_t ei;

	for (ei = 0; ei < prof->nr_edges; ei++) {
		const struct coaccess_edge *e = &prof->edges[ei];
		int ia, ib, ca, cb, i;

		ia = find_field_index(fields, nr, e->field_a);
		ib = find_field_index(fields, nr, e->field_b);
		if (ia < 0 || ib < 0)
			continue;
		if (!field_is_hot(hot, nr_hot, &fields[ia]) ||
		    !field_is_hot(hot, nr_hot, &fields[ib]))
			continue;

		ca = fields[ia].cluster;
		cb = fields[ib].cluster;
		if (ca == cb)
			continue;
		if (cluster_bytes(fields, nr, ca) + cluster_bytes(fields, nr, cb) > (int)max_bytes)
			continue;

		for (i = 0; i < nr; i++) {
			if (fields[i].cluster == cb)
				fields[i].cluster = ca;
		}
	}
}

static void reorder_member_list(struct class *cls, struct class_member **order,
				int nr_order)
{
	struct list_head *head = &cls->type.namespace.tags;
	struct list_head *ins = head;
	struct class_member *m, *n;
	struct list_head new_chain;
	int i;

	type__for_each_data_member(&cls->type, m) {
		ins = m->tag.node.prev;
		break;
	}
	INIT_LIST_HEAD(&new_chain);
	for (i = 0; i < nr_order; i++)
		list_move_tail(&order[i]->tag.node, &new_chain);
	type__for_each_data_member_safe(&cls->type, m, n)
		list_move_tail(&m->tag.node, &new_chain);
	list_splice(&new_chain, ins);
}

static void class__relayout_members(struct class *cls, const struct cu *cu)
{
	struct class_member *m, *last = NULL;
	size_t off = 0;

	type__for_each_data_member(&cls->type, m) {
		size_t align = m->alignment;

		if (align == 0) {
			if (m->byte_size >= cu->addr_size)
				align = cu->addr_size;
			else if (m->byte_size >= 4)
				align = 4;
			else if (m->byte_size >= 2)
				align = 2;
			else
				align = 1;
		}
		off = roundup(off, align);
		m->byte_offset = off;
		m->bit_offset = off * 8 + m->bitfield_offset;
		off += m->byte_size;
		last = m;
	}
	if (last) {
		struct class_member *big =
			type__find_first_biggest_size_base_type_member(&cls->type, cu);
		size_t m_size = big && big->byte_size ? big->byte_size : cu->addr_size;
		size_t unpadded = last->byte_offset + last->byte_size;
		size_t rem = unpadded % m_size;

		if (rem != 0) {
			cls->padding = m_size - rem;
			cls->type.size = unpadded + cls->padding;
		} else {
			cls->padding = 0;
			cls->type.size = unpadded;
		}
	}
	cls->holes_searched = 0;
	class__find_holes(cls);
}

void class__reorganize_coaccess(struct class *cls, const struct cu *cu,
				const struct coaccess_profile *prof,
				size_t cacheline_bytes, unsigned top_pairs,
				int verbose, FILE *fp)
{
	struct field_info fields[MAX_FIELDS];
	char hot[MAX_FIELDS][128];
	struct class_member *order[MAX_FIELDS];
	struct class_member *m;
	int nr = 0, nr_hot = 0, nr_order = 0, i, c;
	int cluster_seen[MAX_FIELDS];
	int cluster_order[MAX_FIELDS];
	int cluster_count = 0;
	unsigned use_pairs;

	if (!prof || prof->nr_edges == 0)
		return;

	nr = collect_fields(cls, cu, fields);
	if (nr == 0)
		return;

	use_pairs = effective_top_pairs(prof, top_pairs, fields, nr, cacheline_bytes);
	mark_hot_fields(prof, top_pairs, fields, nr, cacheline_bytes, hot, &nr_hot);
	if (nr_hot == 0)
		return;

	greedy_affinity_clusters(fields, nr, prof, hot, nr_hot, cacheline_bytes);

	memset(cluster_seen, 0xff, sizeof(cluster_seen));
	for (i = 0; i < nr; i++) {
		if (!field_is_hot(hot, nr_hot, &fields[i]))
			continue;
		c = fields[i].cluster;
		if (cluster_seen[c] < 0) {
			cluster_seen[c] = cluster_count;
			cluster_order[cluster_count++] = c;
		}
	}

	/* Order clusters by total co-access weight so the hottest pair-set lands
	   on line 0 even if those fields were declared late in the struct. */
	for (i = 0; i < cluster_count; i++) {
		int best = i, k;

		for (k = i + 1; k < cluster_count; k++) {
			if (cluster_weight(fields, nr, prof, cluster_order[k]) >
			    cluster_weight(fields, nr, prof, cluster_order[best]))
				best = k;
		}
		if (best != i) {
			int tmp = cluster_order[i];

			cluster_order[i] = cluster_order[best];
			cluster_order[best] = tmp;
		}
	}

	for (c = 0; c < cluster_count; c++) {
		int cid = cluster_order[c];

		for (i = 0; i < nr; i++) {
			if (fields[i].cluster == cid &&
			    field_is_hot(hot, nr_hot, &fields[i]))
				order[nr_order++] = fields[i].member;
		}
	}

	type__for_each_data_member(&cls->type, m) {
		int already = 0;

		if (m->bitfield_size != 0)
			continue;
		for (i = 0; i < nr_order; i++) {
			if (order[i] == m) {
				already = 1;
				break;
			}
		}
		if (!already)
		 order[nr_order++] = m;
	}

	if (verbose) {
		fprintf(fp, "/* coaccess hot-pack: %d hot fields, top_pairs=%u",
			nr_hot, use_pairs);
		if (top_pairs == COACCESS_TOP_PAIRS_AUTO)
			fputs(" (auto)", fp);
		fprintf(fp, ", cacheline=%zu */\n", cacheline_bytes);
	}

	reorder_member_list(cls, order, nr_order);
	class__relayout_members(cls, cu);

	if (verbose > 1) {
		class__fprintf(cls, cu, fp);
		fputc('\n', fp);
	}
}

static int field_cacheline(struct field_info *fields, int nr,
			   const char *name, size_t cacheline_bytes)
{
	int i = find_field_index(fields, nr, name);

	if (i < 0 || cacheline_bytes == 0)
		return -1;
	return (int)(fields[i].member->byte_offset / cacheline_bytes);
}

void coaccess__score(struct class *cls, const struct cu *cu,
		     const struct coaccess_profile *prof,
		     size_t cacheline_bytes, struct coaccess_score *out)
{
	struct field_info fields[MAX_FIELDS];
	int nr;
	size_t i;

	memset(out, 0, sizeof(*out));
	if (!prof || prof->nr_edges == 0 || cacheline_bytes == 0)
		return;

	nr = collect_fields(cls, cu, fields);
	for (i = 0; i < prof->nr_edges; i++) {
		const struct coaccess_edge *e = &prof->edges[i];
		int la = field_cacheline(fields, nr, e->field_a, cacheline_bytes);
		int lb = field_cacheline(fields, nr, e->field_b, cacheline_bytes);

		if (la < 0 || lb < 0)
			continue;
		out->total_edges++;
		out->total_weight += e->weight;
		if (la == lb) {
			out->same_line_edges++;
			out->same_line_weight += e->weight;
		}
	}
}

static double coaccess__pct(long long a, long long b)
{
	return b ? (100.0 * (double)a / (double)b) : 0.0;
}

void coaccess__fprintf_insights(FILE *fp, struct class *before,
				struct class *after, const struct cu *cu,
				const struct coaccess_profile *prof,
				size_t cacheline_bytes, unsigned top_pairs,
				int verbose)
{
	struct coaccess_score sb, sa;
	double before_pct, after_pct;

	if (!prof || prof->nr_edges == 0 || cacheline_bytes == 0)
		return;

	coaccess__score(before, cu, prof, cacheline_bytes, &sb);
	coaccess__score(after, cu, prof, cacheline_bytes, &sa);
	if (sa.total_weight == 0)
		return;

	before_pct = coaccess__pct(sb.same_line_weight, sb.total_weight);
	after_pct = coaccess__pct(sa.same_line_weight, sa.total_weight);

	fprintf(fp, "/* co-access reorder score (weighted, %zu B lines):\n",
		cacheline_bytes);
	fprintf(fp, " *   locality %.1f%% -> %.1f%%  (%+.1f pp)\n",
		before_pct, after_pct, after_pct - before_pct);
	fprintf(fp, " *   pairs sharing a line: %d/%d -> %d/%d\n",
		sb.same_line_edges, sb.total_edges,
		sa.same_line_edges, sa.total_edges);
	fprintf(fp, " *   profile edges resolved: %d/%zu */\n",
		sa.total_edges, prof->nr_edges);

	if (!verbose)
		return;

	{
		struct field_info fb[MAX_FIELDS], fa[MAX_FIELDS];
		int nrb = collect_fields(before, cu, fb);
		int nra = collect_fields(after, cu, fa);
		unsigned use_pairs = effective_top_pairs(prof, top_pairs, fa, nra,
							 cacheline_bytes);
		size_t i;

		fprintf(fp, "/* co-access per-pair (top %u):\n", use_pairs);
		for (i = 0; i < prof->nr_edges && i < use_pairs; i++) {
			const struct coaccess_edge *e = &prof->edges[i];
			int lb_a = field_cacheline(fb, nrb, e->field_a, cacheline_bytes);
			int lb_b = field_cacheline(fb, nrb, e->field_b, cacheline_bytes);
			int la_a = field_cacheline(fa, nra, e->field_a, cacheline_bytes);
			int la_b = field_cacheline(fa, nra, e->field_b, cacheline_bytes);
			const char *tag;

			if (la_a < 0 || la_b < 0)
				tag = "unresolved";
			else if (la_a == la_b)
				tag = (lb_a == lb_b && lb_a >= 0) ? "kept" : "co-located";
			else
				tag = "split";
			fprintf(fp, " *   %s + %s  w=%d  %s\n",
				e->field_a, e->field_b, e->weight, tag);
		}
		fputs(" */\n", fp);
	}
}
