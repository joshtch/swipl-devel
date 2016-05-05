/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  2016, VU University Amsterdam
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "pl-incl.h"
#include "pl-trie.h"
#include "pl-termwalk.c"

static void trie_destroy(trie *trie);

		 /*******************************
		 *	       SYMBOL		*
		 *******************************/

typedef struct tref
{ trie *trie;				/* represented trie */
} tref;

static int
write_trie_ref(IOSTREAM *s, atom_t aref, int flags)
{ tref *ref = PL_blob_data(aref, NULL, NULL);
  (void)flags;

  Sfprintf(s, "<trie>(%p)", ref->trie);
  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
GC a message queue from the atom  garbage collector. This should be fine
because atoms in messages do  not  have   locked  atoms,  so  we are not
calling atom functions.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
release_trie_ref(atom_t aref)
{ tref *ref = PL_blob_data(aref, NULL, NULL);
  trie *t;

  if ( (t=ref->trie) )
  { trie_destroy(t);			/* can be called twice */
    PL_free(t);
  }

  return TRUE;
}


static int
save_trie(atom_t aref, IOSTREAM *fd)
{ tref *ref = PL_blob_data(aref, NULL, NULL);
  (void)fd;

  return PL_warning("Cannot save reference to <trie>(%p)", ref->trie);
}


static atom_t
load_trie(IOSTREAM *fd)
{ (void)fd;

  return PL_new_atom("<saved-trie-ref>");
}


static PL_blob_t trie_blob =
{ PL_BLOB_MAGIC,
  PL_BLOB_UNIQUE,
  "trie",
  release_trie_ref,
  NULL,
  write_trie_ref,
  NULL,
  save_trie,
  load_trie
};

		 /*******************************
		 *	     THE TRIE		*
		 *******************************/

static trie_node *new_trie_node(void);

static trie*
trie_create(void)
{ trie *trie;

  if ( (trie = PL_malloc(sizeof(*trie))) )
  { memset(trie, 0, sizeof(*trie));

    trie->root = new_trie_node();
    return trie;
  } else
  { PL_resource_error("memory");
    return NULL;
  }
}


static void
trie_destroy(trie *trie)
{ Sdprintf("Destroying trie %p\n", trie);
  PL_free(trie);
}


static void
trie_empty(trie *trie)
{
}


static trie_node *
get_child(trie_node *n, word key ARG_LD)
{ trie_children children = n->children;

  if ( children.any )
  { switch( children.any->type )
    { case TN_KEY:
	if ( children.key->key == key )
	  return children.key->child;
        return NULL;
      case TN_HASHED:
	return lookupHTable(children.hash->table, (void*)key);
      default:
	assert(0);
    }
  }

  return NULL;
}


static trie_node *
new_trie_node(void)
{ trie_node *n = PL_malloc(sizeof(*n));

  memset(n, 0, sizeof(*n));
  return n;
}


static void
destroy_hnode(trie_children_hashed *hnode)
{ destroyHTable(hnode->table);
}

static void
destroy_node(trie_node *n)
{ trie_children children = n->children;

  if ( children.any )
  { switch( children.any->type )
    { case TN_KEY:
	break;
      case TN_HASHED:
	destroy_hnode(children.hash);
        break;
    }
  }

  PL_free(n);
}

static void
free_hnode_symbol(void *key, void *value)
{ destroy_node(value);
}


static trie_node *
insert_child(trie_node *n, word key ARG_LD)
{ for(;;)
  { trie_children children = n->children;

    if ( children.any )
    { switch( children.any->type )
      { case TN_KEY:
	{ trie_children_hashed *hnode = PL_malloc(sizeof(*hnode));
	  trie_node *new = new_trie_node();

	  hnode->type  = TN_HASHED;
	  hnode->table = newHTable(4);
	  hnode->table->free_symbol = free_hnode_symbol;
	  addHTable(hnode->table, (void*)key, (void*)new);

	  if ( COMPARE_AND_SWAP(&n->children.hash, NULL, hnode) )
	    return new;
	  destroy_hnode(hnode);
	  continue;
	}
	case TN_HASHED:
	{ trie_node *new = new_trie_node();
	  trie_node *old = addHTable(children.hash->table, (void*)key, (void*)new);

	  if ( new != old )
	    destroy_node(new);
	  return old;
	}
	default:
	  assert(0);
      }
    } else
    { trie_children_key *child = PL_malloc(sizeof(*child));

      child->type  = TN_KEY;
      child->key   = key;
      child->child = new_trie_node();

      if ( COMPARE_AND_SWAP(&n->children.key, NULL, child) )
	return child->child;
      destroy_node(child->child);
      PL_free(child);
    }
  }
}


static trie_node *
follow_node(trie_node *n, word value ARG_LD)
{ trie_node *child;

  if ( (child=get_child(n, value PASS_LD)) )
    return child;

  return insert_child(n, value PASS_LD);
}


static int
trie_insert(trie *trie, Word k, word v ARG_LD)
{ term_agenda agenda;
  Word p;
  trie_node *node = trie->root;

  initTermAgenda(&agenda, 1, k);
  while( (p=nextTermAgenda(&agenda)) )
  { word w = *p;

    switch( tag(w) )
    { case TAG_ATOM:
      { node = follow_node(node, w PASS_LD);
        break;
      }
      case TAG_COMPOUND:
      { Functor f = valueTerm(w);
        int arity = arityFunctor(f->definition);
	node = follow_node(node, f->definition PASS_LD);

	pushWorkAgenda(&agenda, arity, f->arguments);
	break;
      }
      default:
	assert(0);
    }
  }
  clearTermAgenda(&agenda);

  if ( node->value )
  { if ( node->value == v )
      return FALSE;				/* existing */
    return -1;
  }
  node->value = v;

  return TRUE;
}


static trie_node *
trie_lookup(trie *trie, Word k ARG_LD)
{ term_agenda agenda;
  Word p;
  trie_node *node = trie->root;

  initTermAgenda(&agenda, 1, k);
  while( node && (p=nextTermAgenda(&agenda)) )
  { word w = *p;

    switch( tag(w) )
    { case TAG_ATOM:
      { node = get_child(node, w PASS_LD);
        break;
      }
      case TAG_COMPOUND:
      { Functor f = valueTerm(w);
        int arity = arityFunctor(f->definition);
	node = get_child(node, f->definition PASS_LD);

	pushWorkAgenda(&agenda, arity, f->arguments);
	break;
      }
      default:
	assert(0);
    }
  }
  clearTermAgenda(&agenda);

  return node;
}


		 /*******************************
		 *	  PROLOG BINDING	*
		 *******************************/

#define unify_trie(t, trie) unify_trie__LD(t, trie PASS_LD)

static int
unify_trie__LD(term_t t, trie *trie ARG_LD)
{ return PL_unify_atom(t, trie->symbol);
}

static int
get_trie(term_t t, trie **tp)
{ void *data;
  PL_blob_t *type;

  if ( PL_get_blob(t, &data, NULL, &type) && type == &trie_blob )
  { tref *ref = data;

    if ( ref->trie->magic == TRIE_MAGIC )
    { *tp = ref->trie;
      return TRUE;
    }

    return PL_existence_error("trie", t);
  }

  return PL_type_error("trie", t);
}


static
PRED_IMPL("trie_new", 1, trie_new, 0)
{ PRED_LD
  tref ref;

  if ( (ref.trie = trie_create()) )
  { int new;

    ref.trie->symbol = lookupBlob((void*)&ref, sizeof(ref),
				   &trie_blob, &new);
    ref.trie->magic = TRIE_MAGIC;

    return unify_trie(A1, ref.trie);
  }

  return FALSE;
}


static
PRED_IMPL("trie_destroy", 1, trie_destroy, 0)
{ trie *trie;

  if ( get_trie(A1, &trie) )
  { trie_empty(trie);
    trie->magic = TRIE_CMAGIC;

    return TRUE;
  }

  return FALSE;
}


static
PRED_IMPL("trie_insert", 3, trie_insert, 0)
{ PRED_LD
  trie *trie;

  if ( get_trie(A1, &trie) )
  { Word kp, vp;

    kp = valTermRef(A2);
    vp = valTermRef(A3);
    deRef(vp);

    if ( !isAtomic(*vp) || isFloat(*vp) )
      return PL_type_error("primitive", A3);
    if ( isBignum(*vp) )
      return PL_domain_error("primitive", A3);

    return trie_insert(trie, kp, *vp PASS_LD);
  }

  return FALSE;
}


static
PRED_IMPL("trie_lookup", 3, trie_lookup, 0)
{ PRED_LD
  trie *trie;

  if ( get_trie(A1, &trie) )
  { Word kp;
    trie_node *node;

    kp = valTermRef(A2);

    if ( (node = trie_lookup(trie, kp PASS_LD)) &&
	 node->value )
      return _PL_unify_atomic(A3, node->value);
  }

  return FALSE;
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(trie)
  PRED_DEF("trie_new",            1, trie_new,           0)
  PRED_DEF("trie_destroy",        1, trie_destroy,       0)
  PRED_DEF("trie_insert",         3, trie_insert,        0)
  PRED_DEF("trie_lookup",         3, trie_lookup,        0)
EndPredDefs

void
initTries(void)
{ PL_register_blob_type(&trie_blob);
}
