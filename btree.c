#include "bolo.h"

/* reserve the first 8 octets for header data */
#define BTREE_HEADER_SIZE 8

#define BTREE_LEAF 0x80

/* where do the keys start in the mapped page? */
#define BTREE_KEYS_OFFSET (BTREE_HEADER_SIZE)
/* where do the values start in the mapped page? */
#define BTREE_VALS_OFFSET (BTREE_KEYS_OFFSET + BTREE_DEGREE * sizeof(bolo_msec_t))

#define koffset(i) (BTREE_KEYS_OFFSET + (i) * sizeof(bolo_msec_t))
#define voffset(i) (BTREE_VALS_OFFSET + (i) * sizeof(uint64_t))

#define keyat(t,i)   (page_read64(&(t)->page, koffset(i)))
#define valueat(t,i) (page_read64(&(t)->page, voffset(i)))

#define setkeyat(t,i,k)   (page_write64(&(t)->page, koffset(i), k))
#define setvalueat(t,i,v) (page_write64(&(t)->page, voffset(i), v))
#define setchildat(t,i,c) do {\
  setvalueat((t),(i),(c)->id); \
  (t)->kids[(i)] = (c); \
} while (0)

static void
_print(struct btree *t, int indent)
{
	int i;

	assert(t != NULL);
	assert(indent >= 0);
	assert(indent < 7 * 8); /* 7-levels deep seems unlikely... */

	fprintf(stderr, "%*s[btree %p @%lu page %p // %d keys %s]\n",
		indent, "", (void *)t, t->id,  t->page.data, t->used, t->leaf ? "LEAF" : "interior");

	for (i = 0; i < t->used; i++) {
		if (t->leaf) {
			fprintf(stderr, "%*s[%03d] % 10ld / %010lx (= %lu / %010lx)\n",
				indent + 2, "", i, keyat(t,i), keyat(t,i), valueat(t,i), valueat(t,i));
		} else {
			fprintf(stderr, "%*s[%03d] % 10ld / %010lx (%p) -->\n",
				indent + 2, "", i, keyat(t,i), keyat(t,i), (void *)t->kids[i]);
			if (t->kids[i])
				_print(t->kids[i], indent + 8);
		}
	}
	if (t->leaf) {
		fprintf(stderr, "%*s[%03d]          ~ (= %lu / %010lx)\n",
			indent + 2, "", i, valueat(t,t->used), valueat(t,t->used));
	} else {
		fprintf(stderr, "%*s[%03d]          ~ (%p) -->\n",
			indent + 2, "", i, (void *)t->kids[t->used]);
		if (t->kids[t->used])
			_print(t->kids[t->used], indent + 8);
	}
}

void
btree_print(struct btree *bt)
{
	_print(bt, 0);
}

static struct btree *
s_mapat1(int fd, off_t offset)
{
	struct btree *t;

	t = malloc(sizeof(*t));
	if (!t)
		goto fail;

	memset(t, 0, sizeof(*t));
	t->id = (uint64_t)offset;

	if (page_map(&t->page, fd, offset, BTREE_PAGE_SIZE) != 0)
		goto fail;

	t->leaf = page_read8 (&t->page, 5) & BTREE_LEAF;
	t->used = page_read16(&t->page, 6);
	return t;

fail:
	free(t);
	return NULL;
}

static struct btree *
s_mapat(int fd, off_t offset)
{
	struct btree *t;
	int i;

	t = s_mapat1(fd, offset);
	if (!t)
		return NULL;

	if (t->leaf)
		return t;

	for (i = 0; i <= t->used; i++) {
		t->kids[i] = s_mapat(fd, valueat(t,i));
		if (!t->kids[i]) {
			/* clean up the ones that succeeded */
			for (i = i - 1; i >= 0; i--)
				free(t->kids[i]);
			free(t);
			return NULL;
		}
	}

	return t;
}

static struct btree *
s_extend(int fd)
{
	uint64_t id;

	id = lseek(fd, 0, SEEK_END);
	lseek(fd, BTREE_PAGE_SIZE - 1, SEEK_CUR);
	if (write(fd, "\0", 1) != 1)
		return NULL;

	lseek(fd, -1 * BTREE_PAGE_SIZE, SEEK_END);
	assert(BTREE_HEADER_SIZE == 8);
	if (write(fd, "BTREE\x80\x00\x00", BTREE_HEADER_SIZE) != BTREE_HEADER_SIZE)
		return NULL;

	return s_mapat(fd, id);
}

struct btree *
btree_create(int fd)
{
	if (ftruncate(fd, 0) != 0)
		return NULL;

	return s_extend(fd);
}

struct btree *
btree_read(int fd)
{
	off_t offset;
	struct btree *t;

	offset = lseek(fd, 0, SEEK_END);
	if (offset % BTREE_PAGE_SIZE)
		return NULL; /* corrupt or invalid btree */

	t = s_mapat(fd, 0);
	if (!t)
		return NULL;

	return t;
}

static int
s_flush(struct btree *t)
{
	assert(t != NULL);
	assert(t->page.data != NULL);

	page_write8 (&t->page, 5, t->leaf ? BTREE_LEAF : 0);
	page_write16(&t->page, 6, t->used);
	return page_sync(&t->page);
}

int
btree_write(struct btree *t)
{
	int i, rc;

	assert(t != NULL);

	rc = 0;

	if (s_flush(t) != 0)
		rc = -1;

	if (!t->leaf)
		for (i = 0; i <= t->used; i++)
			if (btree_write(t->kids[i]) != 0)
				rc = -1;

	return rc;
}

int
btree_close(struct btree *t)
{
	int i, rc;

	rc = 0;

	if (!t)
		return rc;

	if (!t->leaf)
		for (i = 0; i <= t->used; i++)
			if (btree_close(t->kids[i]) != 0)
				rc = -1;

	if (s_flush(t) != 0)
		rc = -1;

	if (page_unmap(&t->page) != 0)
		rc = -1;
	free(t);

	return rc;
}

static int
s_find(struct btree *t, bolo_msec_t key)
{
	assert(t != NULL);

	int lo, mid, hi;

	lo = -1;
	hi = t->used;
	while (lo + 1 < hi) {
		mid = (lo + hi) / 2;
		if (keyat(t,mid) == key) return mid;
		if (keyat(t,mid) >  key) hi = mid;
		else                     lo = mid;
	}
	return hi;
}

static void
s_shift(struct btree *t, int n)
{
	if (t->used - n <= 0)
		return;

	/* slide all keys above [n] one slot to the right */
	memmove((uint8_t *)t->page.data + koffset(n + 1),
	        (uint8_t *)t->page.data + koffset(n),
	        sizeof(bolo_msec_t) * (t->used - n));

	/* slide all values above [n] one slot to the right */
	memmove((uint8_t *)t->page.data + voffset(n + 2),
	        (uint8_t *)t->page.data + voffset(n + 1),
	        sizeof(uint64_t) * (t->used - n));
	memmove(&t->kids[n + 2],
	        &t->kids[n + 1],
	        sizeof(struct btree*) * (t->used - n));
}

static struct btree *
s_clone(struct btree *t)
{
	struct btree *c;

	c = s_extend(t->page.fd);
	if (!c)
		bail("btree extension failed");

	c->leaf = t->leaf;
	return c;
}

static void
s_divide(struct btree *l, struct btree *r, int mid)
{
	assert(l != NULL);
	assert(r != NULL);
	assert(l != r);
	assert(l->page.data != NULL);
	assert(r->page.data != NULL);
	assert(l->page.data != r->page.data);
	assert(mid != 0);
	assert(l->used >= mid);

	r->used = l->used - mid - 1;
	l->used = mid;

	/* divide the keys at midpoint */
	memmove((uint8_t *)r->page.data + koffset(0),
	        (uint8_t *)l->page.data + koffset(mid + 1),
	        sizeof(bolo_msec_t) * r->used);

	/* divide the values at midpoint */
	memmove((uint8_t *)r->page.data + voffset(0),
	        (uint8_t *)l->page.data + voffset(mid + 1),
	        sizeof(uint64_t) * (r->used + 1));
	memmove(r->kids,
	        &l->kids[mid + 1],
	        sizeof(struct btree *) * (r->used + 1));

	/* note: we don't have to clean up l above mid, so we don't.
	         keep that in mind if you go examining memory in gdb */
#if PEDANTIC
	memset((uint8_t *)l->page.data + koffset(l->used), 0,
	       sizeof(bolo_msec_t) * (BTREE_DEGREE - l->used));
	memset((uint8_t *)l->page.data + voffset(l->used + 1), 0,
	       sizeof(uint64_t)    * (BTREE_DEGREE - l->used + 1));
#endif
}

static struct btree *
s_insert(struct btree *t, bolo_msec_t key, uint64_t block_number, bolo_msec_t *median)
{
	int i, mid;
	struct btree *r;

	assert(t != NULL);
	assert(t->used <= BTREE_DEGREE);
	/* invariant: Each node in the btree will always have enough
	              free space in it to insert at least one value
	              (either a literal, or a node pointer).

	              Splitting is done later in this function (right
	              before returning) as necessary. */

	i = s_find(t, key);

	if (t->leaf) { /* insert into this node */
		if (i < t->used && keyat(t,i) == key) {
			setvalueat(t,i,block_number);
			return NULL;
		}

		s_shift(t, i);
		t->used++;
		setkeyat(t,i,key);
		setvalueat(t,i,block_number);

	} else { /* insert in child */
		if (!t->kids[i])
			t->kids[i] = s_extend(t->page.fd);
		if (!t->kids[i])
			return NULL; /* FIXME this is wrong */

		r = s_insert(t->kids[i], key, block_number, median);
		if (r) {
			s_shift(t, i);
			t->used++;
			setkeyat(t,i,*median);
			setchildat(t,i+1,r);
			return NULL;
		}
	}

	/* split the node now, if it is full, to save complexity */
	if (t->used == BTREE_DEGREE) {
		mid = t->used * BTREE_SPLIT_FACTOR;
		*median = keyat(t,mid);

		r = s_clone(t);
		s_divide(t,r,mid);
		return r;
	}

	return NULL;
}

int
btree_insert(struct btree *t, bolo_msec_t key, uint64_t block_number)
{
	struct btree *l, *r;
	bolo_msec_t m;

	assert(t != NULL);

	r = s_insert(t, key, block_number, &m);
	if (r) {
		/* pivot root to the left */
		l = s_clone(t);
		l->used = t->used;
		l->leaf = t->leaf;
		memmove(l->kids, t->kids, sizeof(t->kids));
		memmove(l->page.data, t->page.data, BTREE_PAGE_SIZE);

		/* re-initialize root as [ l . m . r ] */
		t->used = 1;
		t->leaf = 0;
		setchildat(t, 0, l);
		setkeyat  (t, 0, m);
		setchildat(t, 1, r);
	}

	return 0;
}

uint64_t
btree_find(struct btree *t, bolo_msec_t key)
{
	int i;

	assert(t != NULL);

	i = s_find(t, key);

	if (t->leaf)
		return valueat(t,i);

	if (!t->kids[i])
		return MAX_U64;

	return btree_find(t->kids[i], key);
}


#ifdef TEST
/* Tests will be inserting arbitrary values,
   so we will iterate over a range of keys.
   To generate the values from the keys, we
   will add an arbitrary, non-even constant
   (PERTURB) to make things interesting */
#define PERTURB  0xbad
#define KEYSTART 0x0c00
#define KEYEND   (KEYSTART + 7 * BTREE_DEGREE)

static int
iszero(const void *buf, off_t offset, size_t size)
{
	size_t i;

	assert(buf != NULL);
	assert(offset >= 0);

	for (i = 0; i < size; i++)
		if (((const uint8_t *)buf)[offset + i] != 0)
			return 0; /* found a non-zero; fail */

	return 1; /* checks out! */
}

TESTS {
	int fd;
	struct btree *t, *tmp;
	bolo_msec_t key;
	uint64_t value;

	fd = memfd("btree");
	t = btree_create(fd);
	if (!t)
		BAIL_OUT("btree: btree_create(fd) returned NULL");

	if (!t->leaf)
		BAIL_OUT("btree: initial root node is not considered a leaf");

	if (memcmp(t->page.data, "BTREE\x80\x00\x00", 8) != 0)
		BAIL_OUT("btree: initial root node header incorrect");

	if (!iszero(t->page.data, 8, BTREE_PAGE_SIZE - 8))
		BAIL_OUT("btree: initial root node (less header) should be zeroed (but wasn't!)");

	is_unsigned(lseek(fd, 0, SEEK_END), BTREE_PAGE_SIZE,
		"new btree file should be exactly ONE page long");

	for (key = KEYSTART; key <= KEYEND; key++) {
		if (btree_insert(t, key, key + PERTURB) != 0)
			fail("failed to insert %#lx => %#lx", key, key + PERTURB);
	}
	pass("btree insertions should succeed");

	for (key = KEYSTART; key <= KEYEND; key++) {
		value = btree_find(t, key);
		if (value != key + PERTURB)
			is_unsigned(value, key + PERTURB, "btree lookup(%lx)", key);
	}
	pass("btree lookups should succeed");

	if (btree_write(t) != 0)
		BAIL_OUT("btree_write failed");

	tmp = t;
	t = btree_read(fd);
	if (!t)
		BAIL_OUT("btree_read(fd) failed!");

	for (key = KEYSTART; key <= KEYEND; key++) {
		value = btree_find(t, key);
		if (value != key + PERTURB)
			is_unsigned(value, key + PERTURB, "btree lookup(%lx) after re-read", key);
	}
	pass("btree lookups after re-read should succeed");


	if (btree_close(t) != 0)
		BAIL_OUT("btree_close failed");

	btree_close(tmp);
	close(fd);
}

#endif
