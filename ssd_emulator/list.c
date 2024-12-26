//#include <stdint.h>
#include "nvmev.h"
#include "list.h"

//#include <assert.h>
//#include <stdio.h>

/* Our doubly linked lists have two header elements: the "head"
   just before the first element and the "tail" just after the
   last element.  The `prev' link of the front header is null, as
   is the `next' link of the back header.  Their other two links
   point toward each other via the interior elements of the list.

   An empty list looks like this:

                      +------+     +------+
                  <---| head |<--->| tail |--->
                      +------+     +------+

   A list with two elements in it looks like this:

        +------+     +-------+     +-------+     +------+
    <---| head |<--->|   1   |<--->|   2   |<--->| tail |<--->
        +------+     +-------+     +-------+     +------+

   The symmetry of this arrangement eliminates lots of special
   cases in list processing.  For example, take a look at
   list_remove(): it takes only two pointer assignments and no
   conditionals.  That's a lot simpler than the code would be
   without header elements.

   (Because only one of the pointers in each header element is used,
   we could in fact combine them into a single header element
   without sacrificing this simplicity.  But using two separate
   elements allows us to do a little bit of checking on some
   operations, which can be valuable.) */

/* Returns true if ELEM is an interior element,
   false otherwise. */

#define assert_(x) \
	BUG_ON((!(x)))

bool is_interior(struct list_elem *elem)
{
  return elem != NULL && elem->prev != NULL && elem->next != NULL;
}


/* Returns true if ELEM is a tail, false otherwise. */
static inline bool is_tail(struct list_elem *elem)
{
  return elem != NULL && elem->prev != NULL && elem->next == NULL;
}

static inline bool is_head(struct list_elem *elem)
{
  return elem != NULL && elem->prev == NULL && elem->next != NULL;
}

/* Initializes LIST as an empty list. */
void list_init(struct list *list)
{
  //assert(list != NULL);
  assert_(list != NULL);
  //NVMEV_ASSERT(list != NULL);
  list->head.prev = NULL;
  list->head.next = &list->tail;
  list->tail.prev = &list->head;
  list->tail.next = NULL;
}

/* Returns the beginning of LIST.  */
struct list_elem *list_begin(struct list *list)
{
  assert_(list != NULL);
  return list->head.next;
}

/* Returns LIST's tail.

   list_end() is often used in iterating through a list from
   front to back.  See the big comment at the top of list.h for
   an example. */
struct list_elem *list_end(struct list *list)
{
  assert_(list != NULL);
  return &list->tail;
}

/* Inserts ELEM just before BEFORE, which may be either an
   interior element or a tail.  The latter case is equivalent to
   list_push_back(). */
void list_insert(struct list_elem *before, struct list_elem *elem)
{
  assert_(is_interior(before) || is_tail(before));
  assert_(elem != NULL);

  elem->prev = before->prev;
  elem->next = before;
  before->prev->next = elem;
  before->prev = elem;
}

struct list_elem * list_before(struct list_elem *elem)
{
  assert_(elem != NULL);
  struct list_elem *before;
  before = elem->prev;
  
  if (is_head(before))
	  return NULL;
  return before;
}

struct list_elem * list_next(struct list_elem *elem)
{
  assert_(elem != NULL);
  struct list_elem *next;
  next = elem->next;
  
  if (is_tail(next))
	  return NULL;
  return next;
}

/* Inserts ELEM at the beginning of LIST, so that it becomes the
   front in LIST. */
void list_push_front(struct list *list, struct list_elem *elem)
{
  list_insert(list_begin(list), elem);
}

/* Inserts ELEM at the end of LIST, so that it becomes the
   back in LIST. */
void list_push_back(struct list *list, struct list_elem *elem)
{
  list_insert(list_end(list), elem);
}

/* Removes ELEM from its list and returns the element that
   followed it.  Undefined behavior if ELEM is not in a list.

   A list element must be treated very carefully after removing
   it from its list.  Calling list_next() or list_prev() on ELEM
   will return the item that was previously before or after ELEM,
   but, e.g., list_prev(list_next(ELEM)) is no longer ELEM!

   The list_remove() return value provides a convenient way to
   iterate and remove elements from a list:

   for (e = list_begin (&list); e != list_end (&list); e = list_remove (e))
     {
       ...do something with e...
     }

   If you need to free() elements of the list then you need to be
   more conservative.  Here's an alternate strategy that works
   even in that case:

   while (!list_empty (&list))
     {
       struct list_elem *e = list_pop_front (&list);
       ...do something with e...
     }
*/
struct list_elem *list_remove(struct list_elem *elem)
{
  struct list_elem *ret;
  assert_(is_interior(elem));
  elem->prev->next = elem->next;
  elem->next->prev = elem->prev;
  ret = elem->next;
  elem->next = NULL;
  elem->prev = NULL;
  return ret;
}

/* Removes the front element from LIST and returns it.
   Undefined behavior if LIST is empty before removal. */
struct list_elem *list_pop_front(struct list *list)
{
  struct list_elem *front = list_front(list);
  list_remove(front);
  return front;
}


/* Returns true if LIST is empty, false otherwise. */
bool list_empty_(struct list *list)
{
  return list_begin(list) == list_end(list);
}


/* Returns the front element in LIST.
   Undefined behavior if LIST is empty. */
struct list_elem * list_front(struct list *list)
{
  assert_(!list_empty_(list));
  return list->head.next;
}


struct list_elem * list_last(struct list *list)
{
  assert_(!list_empty_(list));
  return list->tail.prev;
}


void change_list(struct list *old_list, struct list *new_list) 
{
  struct list_elem *first_le, *last_le;
  
  assert_(list_empty_(new_list));
  assert_(!list_empty_(old_list));

  first_le = list_front(old_list);
  last_le = list_last(old_list);
  
  new_list->head.next = first_le;
  first_le->prev = &new_list->head;
  
  new_list->tail.prev = last_le;
  last_le->next = &new_list->tail;
  
  list_init(old_list);

}

