// SPDX-License-Identifier: GPL-2.0
/*
 * Synthetic struct for coaccess reorg tests: hot scalars + anonymous union.
 */
struct coaccess_test {
	int cold_a;
	int cold_b;
	int hot_x;
	int hot_y;
	union {
		int donor;
		int curr;
	};
	int cold_c;
};

int coaccess_test_use(struct coaccess_test *t)
{
	return t->cold_a + t->hot_x + t->hot_y + t->donor + t->curr + t->cold_c;
}
