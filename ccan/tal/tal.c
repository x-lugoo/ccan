/* Licensed under BSD-MIT - see LICENSE file for details */
#include <ccan/tal/tal.h>
#include <ccan/compiler/compiler.h>
#include <ccan/list/list.h>
#include <ccan/take/take.h>
#include <ccan/alignof/alignof.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

//#define TAL_DEBUG 1

#define NOTIFY_IS_DESTRUCTOR 512

/* 32-bit type field, first byte 0 in either endianness. */
enum prop_type {
	CHILDREN = 0x00c1d500,
	NAME = 0x00111100,
	NOTIFIER = 0x00071f00,
	LENGTH = 0x00515300
};

struct tal_hdr {
	struct list_node list;
	struct prop_hdr *prop;
	struct children *parent_child;
};

struct prop_hdr {
	enum prop_type type;
	struct prop_hdr *next;
};

struct children {
	struct prop_hdr hdr; /* CHILDREN */
	struct tal_hdr *parent;
	struct list_head children; /* Head of siblings. */
};

struct name {
	struct prop_hdr hdr; /* NAME */
	char name[];
};

struct length {
	struct prop_hdr hdr; /* LENGTH */
	size_t count;
};

struct notifier {
	struct prop_hdr hdr; /* NOTIFIER */
	enum tal_notify_type types;
	union {
		void (*notifyfn)(tal_t *, enum tal_notify_type, void *);
		void (*destroy)(tal_t *); /* If NOTIFY_IS_DESTRUCTOR set */
	} u;
};

static struct {
	struct tal_hdr hdr;
	struct children c;
} null_parent = { { { &null_parent.hdr.list, &null_parent.hdr.list },
		    &null_parent.c.hdr, NULL },
		  { { CHILDREN, NULL },
		    &null_parent.hdr,
		    { { &null_parent.c.children.n,
			&null_parent.c.children.n } }
		  }
};


static void *(*allocfn)(size_t size) = malloc;
static void *(*resizefn)(void *, size_t size) = realloc;
static void (*freefn)(void *) = free;
static void (*errorfn)(const char *msg) = (void *)abort;
static bool initialized = false;
/* Count on non-destrutor notifiers; often stays zero. */
static size_t notifiers = 0;

static inline void COLD call_error(const char *msg)
{
	errorfn(msg);
}

static bool get_destroying_bit(struct children *parent_child)
{
	return (size_t)parent_child & 1;
}

static void set_destroying_bit(struct children **parent_child)
{
	*parent_child = (void *)((size_t)*parent_child | 1);
}

static struct children *ignore_destroying_bit(struct children *parent_child)
{
	return (void *)((size_t)parent_child & ~(size_t)1);
}

/* This means valgrind can see leaks. */
static void tal_cleanup(void)
{
	struct tal_hdr *i;

	while ((i = list_top(&null_parent.c.children, struct tal_hdr, list)))
		list_del(&i->list);

	/* Cleanup any taken pointers. */
	take_cleanup();
}

/* For allocation failures inside ccan/take */
static void take_alloc_failed(const void *p)
{
	tal_free(p);
}

/* We carefully start all real properties with a zero byte. */
static bool is_literal(const struct prop_hdr *prop)
{
	return ((char *)prop)[0] != 0;
}

#ifndef NDEBUG
static const void *bounds_start, *bounds_end;

static void update_bounds(const void *new, size_t size)
{
	if (unlikely(!bounds_start)) {
		bounds_start = new;
		bounds_end = (char *)new + size;
	} else if (new < bounds_start)
		bounds_start = new;
	else if ((char *)new + size > (char *)bounds_end)
		bounds_end = (char *)new + size;
}

static bool in_bounds(const void *p)
{
	return !p
		|| (p >= (void *)&null_parent && p <= (void *)(&null_parent + 1))
		|| (p >= bounds_start && p <= bounds_end);
}
#else
static void update_bounds(const void *new, size_t size)
{
}

static bool in_bounds(const void *p)
{
	return true;
}
#endif

static void check_bounds(const void *p)
{
	if (!in_bounds(p))
		call_error("Not a valid header");
}

static struct tal_hdr *to_tal_hdr(const void *ctx)
{
	struct tal_hdr *t;

	t = (struct tal_hdr *)((char *)ctx - sizeof(struct tal_hdr));
	check_bounds(t);
	check_bounds(ignore_destroying_bit(t->parent_child));
	check_bounds(t->list.next);
	check_bounds(t->list.prev);
	if (t->prop && !is_literal(t->prop))
		check_bounds(t->prop);
	return t;
}

static struct tal_hdr *to_tal_hdr_or_null(const void *ctx)
{
	if (!ctx)
		return &null_parent.hdr;
	return to_tal_hdr(ctx);
}

static void *from_tal_hdr(const struct tal_hdr *hdr)
{
	return (void *)(hdr + 1);
}

#ifdef TAL_DEBUG
static void *from_tal_hdr_or_null(struct tal_hdr *hdr)
{
	if (hdr == &null_parent.hdr)
		return NULL;
	return from_tal_hdr(hdr);
}

static struct tal_hdr *debug_tal(struct tal_hdr *tal)
{
	tal_check(from_tal_hdr_or_null(tal), "TAL_DEBUG ");
	return tal;
}
#else
static struct tal_hdr *debug_tal(struct tal_hdr *tal)
{
	return tal;
}
#endif

static void notify(const struct tal_hdr *ctx,
		   enum tal_notify_type type, const void *info)
{
        const struct prop_hdr *p;

        for (p = ctx->prop; p; p = p->next) {
		struct notifier *n;

                if (is_literal(p))
			break;
                if (p->type != NOTIFIER)
			continue;
		n = (struct notifier *)p;
		if (n->types & type) {
			if (n->types & NOTIFY_IS_DESTRUCTOR)
				n->u.destroy(from_tal_hdr(ctx));
			else
				n->u.notifyfn(from_tal_hdr(ctx), type,
					      (void *)info);
		}
	}
}

static void *allocate(size_t size)
{
	void *ret = allocfn(size);
	if (!ret)
		call_error("allocation failed");
	else
		update_bounds(ret, size);
	return ret;
}

static struct prop_hdr **find_property_ptr(const struct tal_hdr *t,
					   enum prop_type type)
{
        struct prop_hdr **p;

        for (p = (struct prop_hdr **)&t->prop; *p; p = &(*p)->next) {
                if (is_literal(*p)) {
                        if (type == NAME)
                                return p;
                        break;
                }
                if ((*p)->type == type)
                        return p;
        }
        return NULL;
}

static void *find_property(const struct tal_hdr *parent, enum prop_type type)
{
        struct prop_hdr **p = find_property_ptr(parent, type);

        if (p)
                return *p;
        return NULL;
}

static void init_property(struct prop_hdr *hdr,
			  struct tal_hdr *parent,
			  enum prop_type type)
{
	hdr->type = type;
	hdr->next = parent->prop;
	parent->prop = hdr;
}

static struct notifier *add_notifier_property(struct tal_hdr *t,
					      enum tal_notify_type types,
					      void (*fn)(void *,
							 enum tal_notify_type,
							 void *))
{
	struct notifier *prop = allocate(sizeof(*prop));
	if (prop) {
		init_property(&prop->hdr, t, NOTIFIER);
		prop->types = types;
		prop->u.notifyfn = fn;
	}
	return prop;
}

static enum tal_notify_type del_notifier_property(struct tal_hdr *t,
						  void (*fn)(tal_t *,
							     enum tal_notify_type,
							     void *))
{
        struct prop_hdr **p;

        for (p = (struct prop_hdr **)&t->prop; *p; p = &(*p)->next) {
		struct notifier *n;

                if (is_literal(*p))
			break;
                if ((*p)->type != NOTIFIER)
			continue;
		n = (struct notifier *)*p;
		if (n->u.notifyfn == fn) {
			enum tal_notify_type types = n->types;
			*p = (*p)->next;
			freefn(n);
			return types & ~NOTIFY_IS_DESTRUCTOR;
		}
        }
        return 0;
}

static struct name *add_name_property(struct tal_hdr *t, const char *name)
{
	struct name *prop;

	prop = allocate(sizeof(*prop) + strlen(name) + 1);
	if (prop) {
		init_property(&prop->hdr, t, NAME);
		strcpy(prop->name, name);
	}
	return prop;
}

static struct children *add_child_property(struct tal_hdr *parent,
					   struct tal_hdr *child)
{
	struct children *prop = allocate(sizeof(*prop));
	if (prop) {
		init_property(&prop->hdr, parent, CHILDREN);
		prop->parent = parent;
		list_head_init(&prop->children);
	}
	return prop;
}

static bool add_child(struct tal_hdr *parent, struct tal_hdr *child)
{
	struct children *children = find_property(parent, CHILDREN);

        if (!children) {
		if (unlikely(!initialized)) {
			atexit(tal_cleanup);
			take_allocfail(take_alloc_failed);
			initialized = true;
		}
		children = add_child_property(parent, child);
		if (!children)
			return false;
	}
	list_add(&children->children, &child->list);
	child->parent_child = children;
	return true;
}

static void del_tree(struct tal_hdr *t, const tal_t *orig)
{
	struct prop_hdr **prop, *p, *next;

        /* Already being destroyed?  Don't loop. */
        if (unlikely(get_destroying_bit(t->parent_child)))
                return;

        set_destroying_bit(&t->parent_child);

	/* Call free notifiers. */
	notify(t, TAL_NOTIFY_FREE, (tal_t *)orig);

	/* Now free children and groups. */
	prop = find_property_ptr(t, CHILDREN);
	if (prop) {
		struct tal_hdr *i;
		struct children *c = (struct children *)*prop;

		while ((i = list_top(&c->children, struct tal_hdr, list))) {
			list_del(&i->list);
			del_tree(i, orig);
		}
	}

        /* Finally free our properties. */
        for (p = t->prop; p && !is_literal(p); p = next) {
                next = p->next;
		/* LENGTH is appended, so don't free separately! */
		if (p->type != LENGTH)
			freefn(p);
        }
        freefn(t);
}

void *tal_alloc_(const tal_t *ctx, size_t size, bool clear, const char *label)
{
        struct tal_hdr *child, *parent = debug_tal(to_tal_hdr_or_null(ctx));

        child = allocate(sizeof(struct tal_hdr) + size);
	if (!child)
		return NULL;
	if (clear)
		memset(from_tal_hdr(child), 0, size);
        child->prop = (void *)label;
        if (!add_child(parent, child)) {
		freefn(child);
		return NULL;
	}
	debug_tal(parent);
	if (notifiers)
		notify(parent, TAL_NOTIFY_ADD_CHILD, from_tal_hdr(child));
	return from_tal_hdr(debug_tal(child));
}

static bool adjust_size(size_t *size, size_t count)
{
	const size_t extra = sizeof(struct tal_hdr) + sizeof(struct length)*2;

	/* Multiplication wrap */
        if (count && unlikely(*size * count / *size != count))
		goto overflow;

        *size *= count;

        /* Make sure we don't wrap adding header/tailer. */
        if (*size + extra < extra)
		goto overflow;
	return true;
overflow:
	call_error("allocation size overflow");
	return false;
}

static size_t extra_for_length(size_t size)
{
	size_t extra;
	const size_t align = ALIGNOF(struct length);

	/* Round up size, and add tailer. */
	extra = ((size + align-1) & ~(align-1)) - size;
	extra += sizeof(struct length);
	return extra;
}

void *tal_alloc_arr_(const tal_t *ctx, size_t size, size_t count, bool clear,
		     bool add_count, const char *label)
{
	void *ret;

	if (!adjust_size(&size, count))
		return NULL;

	if (add_count)
		size += extra_for_length(size);

	ret = tal_alloc_(ctx, size, clear, label);
	if (unlikely(!ret))
		return ret;

	if (add_count) {
		struct length *lprop;
		lprop = (struct length *)((char *)ret + size) - 1;
		init_property(&lprop->hdr, to_tal_hdr(ret), LENGTH);
		lprop->count = count;
	}
	return ret;
}

void *tal_free(const tal_t *ctx)
{
        if (ctx) {
		struct tal_hdr *t;
		int saved_errno = errno;
		t = debug_tal(to_tal_hdr(ctx));
		if (notifiers)
			notify(ignore_destroying_bit(t->parent_child)->parent,
			       TAL_NOTIFY_DEL_CHILD, ctx);
		list_del(&t->list);
		del_tree(t, ctx);
		errno = saved_errno;
	}
	return NULL;
}

void *tal_steal_(const tal_t *new_parent, const tal_t *ctx)
{
        if (ctx) {
		struct tal_hdr *newpar, *t, *old_parent;

                newpar = debug_tal(to_tal_hdr_or_null(new_parent));
                t = debug_tal(to_tal_hdr(ctx));

                /* Unlink it from old parent. */
		list_del(&t->list);
		old_parent = ignore_destroying_bit(t->parent_child)->parent;

                if (unlikely(!add_child(newpar, t))) {
			/* We can always add to old parent, becuase it has a
			 * children property already. */
			if (!add_child(old_parent, t))
				abort();
			return NULL;
		}
		debug_tal(newpar);
		if (notifiers)
			notify(t, TAL_NOTIFY_STEAL, new_parent);
        }
        return (void *)ctx;
}

bool tal_add_destructor_(const tal_t *ctx, void (*destroy)(void *me))
{
	tal_t *t = debug_tal(to_tal_hdr(ctx));
	return add_notifier_property(t, TAL_NOTIFY_FREE|NOTIFY_IS_DESTRUCTOR,
				     (void *)destroy);
}

bool tal_add_notifier_(const tal_t *ctx, enum tal_notify_type types,
		       void (*callback)(tal_t *, enum tal_notify_type, void *))
{
	tal_t *t = debug_tal(to_tal_hdr(ctx));
	struct notifier *n;

	assert(types);
	assert((types & ~(TAL_NOTIFY_FREE | TAL_NOTIFY_STEAL | TAL_NOTIFY_MOVE
			  | TAL_NOTIFY_RESIZE | TAL_NOTIFY_RENAME
			  | TAL_NOTIFY_ADD_CHILD | TAL_NOTIFY_DEL_CHILD
			  | TAL_NOTIFY_ADD_NOTIFIER
			  | TAL_NOTIFY_DEL_NOTIFIER)) == 0);

	/* Don't call notifier about itself: set types after! */
        n = add_notifier_property(t, 0, callback);
	if (unlikely(!n))
		return false;

	if (notifiers)
		notify(t, TAL_NOTIFY_ADD_NOTIFIER, callback);

	n->types = types;
	if (types != TAL_NOTIFY_FREE)
		notifiers++;
	return true;
}

bool tal_del_notifier_(const tal_t *ctx,
		       void (*callback)(tal_t *, enum tal_notify_type, void *))
{
	struct tal_hdr *t = debug_tal(to_tal_hdr(ctx));
	enum tal_notify_type types;

        types = del_notifier_property(t, callback);
	if (types) {
		notify(t, TAL_NOTIFY_DEL_NOTIFIER, callback);
		if (types != TAL_NOTIFY_FREE)
			notifiers--;
		return true;
	}
	return false;
}

bool tal_del_destructor_(const tal_t *ctx, void (*destroy)(void *me))
{
	return tal_del_notifier_(ctx, (void *)destroy);
}

bool tal_set_name_(tal_t *ctx, const char *name, bool literal)
{
        struct tal_hdr *t = debug_tal(to_tal_hdr(ctx));
        struct prop_hdr **prop = find_property_ptr(t, NAME);

        /* Get rid of any old name */
        if (prop) {
                struct name *name = (struct name *)*prop;
                if (is_literal(&name->hdr))
                        *prop = NULL;
                else {
                        *prop = name->hdr.next;
			freefn(name);
                }
        }

        if (literal && name[0]) {
                struct prop_hdr **p;

                /* Append literal. */
                for (p = &t->prop; *p && !is_literal(*p); p = &(*p)->next);
                *p = (struct prop_hdr *)name;
        } else if (!add_name_property(t, name))
		return false;

	debug_tal(t);
	if (notifiers)
		notify(t, TAL_NOTIFY_RENAME, name);
	return true;
}

const char *tal_name(const tal_t *t)
{
        struct name *n;

	n = find_property(debug_tal(to_tal_hdr(t)), NAME);
	if (!n)
		return NULL;

	if (is_literal(&n->hdr))
		return (const char *)n;
	return n->name;
}

size_t tal_count(const tal_t *ptr)
{
	struct length *l;

	l = find_property(debug_tal(to_tal_hdr(ptr)), LENGTH);
	if (!l)
		return 0;
	return l->count;
}

/* Start one past first child: make stopping natural in circ. list. */
static struct tal_hdr *first_child(struct tal_hdr *parent)
{
	struct children *child;

	child = find_property(parent, CHILDREN);
        if (!child)
                return NULL;

	return list_top(&child->children, struct tal_hdr, list);
}

tal_t *tal_first(const tal_t *root)
{
        struct tal_hdr *c, *t = debug_tal(to_tal_hdr_or_null(root));

	c = first_child(t);
	if (!c)
		return NULL;
	return from_tal_hdr(c);
}

tal_t *tal_next(const tal_t *root, const tal_t *prev)
{
        struct tal_hdr *c, *t = debug_tal(to_tal_hdr(prev)), *top;

        /* Children? */
	c = first_child(t);
	if (c)
		return from_tal_hdr(c);

        top = to_tal_hdr_or_null(root);
        do {
		struct tal_hdr *next;
		struct list_node *end;

		end = &ignore_destroying_bit(t->parent_child)->children.n;

		next = list_entry(t->list.next, struct tal_hdr, list);
		if (&next->list != end)
			return from_tal_hdr(next);

                /* OK, go back to parent. */
                t = ignore_destroying_bit(t->parent_child)->parent;
        } while (t != top);

        return NULL;
}

tal_t *tal_parent(const tal_t *ctx)
{
        struct tal_hdr *t;

	if (!ctx)
		return NULL;

	t = debug_tal(to_tal_hdr(ctx));
	if (ignore_destroying_bit(t->parent_child)->parent == &null_parent.hdr)
		return NULL;
        return from_tal_hdr(ignore_destroying_bit(t->parent_child)->parent);
}

bool tal_resize_(tal_t **ctxp, size_t size, size_t count)
{
        struct tal_hdr *old_t, *t;
        struct children *child;
	struct prop_hdr **lenp;
	struct length len;
	size_t extra = 0;

        old_t = debug_tal(to_tal_hdr(*ctxp));

	if (!adjust_size(&size, count))
		return false;

	lenp = find_property_ptr(old_t, LENGTH);
	if (lenp) {
		/* Copy here, in case we're shrinking! */
		len = *(struct length *)*lenp;
		extra = extra_for_length(size);
	}

        t = resizefn(old_t, sizeof(struct tal_hdr) + size + extra);
	if (!t) {
		call_error("Reallocation failure");
		return false;
	}

	/* Copy length to end. */
	if (lenp) {
		struct length *new_len;

		new_len = (struct length *)((char *)(t + 1) + size);
		len.count = count;
		*new_len = len;

		/* Be careful replacing next ptr; could be old hdr. */
		if (lenp == &old_t->prop)
			t->prop = &new_len->hdr;
		else
			*lenp = &new_len->hdr;
	}

	update_bounds(t, sizeof(struct tal_hdr) + size + extra);

	/* If it didn't move, we're done! */
        if (t != old_t) {
		/* Fix up linked list pointers. */
		t->list.next->prev = t->list.prev->next = &t->list;

		/* Fix up child property's parent pointer. */
		child = find_property(t, CHILDREN);
		if (child) {
			assert(child->parent == old_t);
			child->parent = t;
		}
		*ctxp = from_tal_hdr(debug_tal(t));
		if (notifiers)
			notify(t, TAL_NOTIFY_MOVE, from_tal_hdr(old_t));
	}
	if (notifiers)
		notify(t, TAL_NOTIFY_RESIZE, (void *)size);

	return true;
}

bool tal_expand_(tal_t **ctxp, const void *src, size_t size, size_t count)
{
	struct length *l;
	size_t old_count;
	bool ret = false;

	l = find_property(debug_tal(to_tal_hdr(*ctxp)), LENGTH);
	old_count = l->count;

	/* Check for additive overflow */
	if (old_count + count < count) {
		call_error("dup size overflow");
		goto out;
	}

	/* Don't point src inside thing we're expanding! */
	assert(src < *ctxp
	       || (char *)src >= (char *)(*ctxp) + (size * old_count));

	if (!tal_resize_(ctxp, size, old_count + count))
		goto out;

	memcpy((char *)*ctxp + size * old_count, src, count * size);
	ret = true;

out:
	if (taken(src))
		tal_free(src);
	return ret;
}

void *tal_dup_(const tal_t *ctx, const void *p, size_t size,
	       size_t n, size_t extra, bool add_count,
	       const char *label)
{
	void *ret;
	size_t nbytes = size;

	if (!adjust_size(&nbytes, n)) {
		if (taken(p))
			tal_free(p);
		return NULL;
	}

	/* Beware addition overflow! */
	if (n + extra < n) {
		call_error("dup size overflow");
		if (taken(p))
			tal_free(p);
		return NULL;
	}

	if (taken(p)) {
		if (unlikely(!p))
			return NULL;
		if (unlikely(!tal_resize_((void **)&p, size, n + extra)))
			return tal_free(p);
		if (unlikely(!tal_steal(ctx, p)))
			return tal_free(p);
		return (void *)p;
	}

	ret = tal_alloc_arr_(ctx, size, n + extra, false, add_count, label);
	if (ret)
		memcpy(ret, p, nbytes);
	return ret;
}

void tal_set_backend(void *(*alloc_fn)(size_t size),
		     void *(*resize_fn)(void *, size_t size),
		     void (*free_fn)(void *),
		     void (*error_fn)(const char *msg))
{
	if (alloc_fn)
		allocfn = alloc_fn;
	if (resize_fn)
		resizefn = resize_fn;
	if (free_fn)
		freefn = free_fn;
	if (error_fn)
		errorfn = error_fn;
}

#ifdef CCAN_TAL_DEBUG
static void dump_node(unsigned int indent, const struct tal_hdr *t)
{
	unsigned int i;
        const struct prop_hdr *p;

	for (i = 0; i < indent; i++)
		printf("  ");
	printf("%p", t);
        for (p = t->prop; p; p = p->next) {
		struct children *c;
		struct name *n;
		struct notifier *no;
		struct length *l;
                if (is_literal(p)) {
			printf(" \"%s\"", (const char *)p);
			break;
		}
		switch (p->type) {
		case CHILDREN:
			c = (struct children *)p;
			printf(" CHILDREN(%p):parent=%p,children={%p,%p}\n",
			       p, c->parent,
			       c->children.n.prev, c->children.n.next);
			break;
		case NAME:
			n = (struct name *)p;
			printf(" NAME(%p):%s", p, n->name);
			break;
		case NOTIFIER:
			no = (struct notifier *)p;
			printf(" NOTIFIER(%p):fn=%p", p, no->u.notifyfn);
			break;
		case LENGTH:
			l = (struct length *)p;
			printf(" LENGTH(%p):count=%zu", p, l->count);
			break;
		default:
			printf(" **UNKNOWN(%p):%i**", p, p->type);
		}
	}
	printf("\n");
}

static void tal_dump_(unsigned int level, const struct tal_hdr *t)
{
        struct children *children;

	dump_node(level, t);

	children = find_property(t, CHILDREN);
	if (children) {
		struct tal_hdr *i;

		list_for_each(&children->children, i, list)
			tal_dump_(level + 1, i);
	}
}

void tal_dump(void)
{
	tal_dump_(0, &null_parent.hdr);
}
#endif /* CCAN_TAL_DEBUG */

#ifndef NDEBUG
static bool check_err(struct tal_hdr *t, const char *errorstr,
		      const char *errmsg)
{
	if (errorstr) {
		/* Try not to malloc: it may be corrupted. */
		char msg[strlen(errorstr) + 20 + strlen(errmsg) + 1];
		sprintf(msg, "%s:%p %s", errorstr, from_tal_hdr(t), errmsg);
		call_error(msg);
	}
	return false;
}

static bool check_node(struct children *parent_child,
		       struct tal_hdr *t, const char *errorstr)
{
	struct prop_hdr *p;
	struct name *name = NULL;
	struct children *children = NULL;
	struct length *length = NULL;

	if (!in_bounds(t))
		return check_err(t, errorstr, "invalid pointer");

	if (ignore_destroying_bit(t->parent_child) != parent_child)
		return check_err(t, errorstr, "incorrect parent");

	for (p = t->prop; p; p = p->next) {
		if (is_literal(p)) {
			if (name)
				return check_err(t, errorstr,
						 "has extra literal");
			name = (struct name *)p;
			break;
		}
		if (!in_bounds(p))
			return check_err(t, errorstr,
					 "has bad property pointer");

		switch (p->type) {
		case CHILDREN:
			if (children)
				return check_err(t, errorstr,
						 "has two child nodes");
			children = (struct children *)p;
			break;
		case LENGTH:
			if (length)
				return check_err(t, errorstr,
						 "has two lengths");
			length = (struct length *)p;
			break;
		case NOTIFIER:
			break;
		case NAME:
			if (name)
				return check_err(t, errorstr,
						 "has two names");
			name = (struct name *)p;
			break;
		default:
			return check_err(t, errorstr, "has unknown property");
		}
	}
	if (children) {
		struct tal_hdr *i;

		if (!list_check(&children->children, errorstr))
			return false;
		list_for_each(&children->children, i, list) {
			if (!check_node(children, i, errorstr))
				return false;
		}
	}
	return true;
}

bool tal_check(const tal_t *ctx, const char *errorstr)
{
	struct tal_hdr *t = to_tal_hdr_or_null(ctx);

	return check_node(ignore_destroying_bit(t->parent_child), t, errorstr);
}
#else /* NDEBUG */
bool tal_check(const tal_t *ctx, const char *errorstr)
{
	return true;
}
#endif
