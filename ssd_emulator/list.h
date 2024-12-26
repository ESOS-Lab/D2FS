#ifndef LIST_H
#define LIST_H

#include <stdbool.h>
#include <stddef.h>
//#include <stdint.h>


/* List element. */
struct list_elem 
{
  struct list_elem *prev;     /* Previous list element. */
  struct list_elem *next;     /* Next list element. */
};

/* List. */
struct list 
{
  struct list_elem head;      /* List head. */
  struct list_elem tail;      /* List tail. */
};

/* Converts pointer to list element LIST_ELEM into a pointer to
   the structure that LIST_ELEM is embedded inside.  Supply the
   name of the outer structure STRUCT and the member name MEMBER
   of the list element.  See the big comment at the top of the
   file for an example. */
/*#define list_entry(LIST_ELEM, STRUCT, MEMBER)           \
        ((STRUCT *) ((uint8_t *) &(LIST_ELEM)->next     \
                     - offsetof (STRUCT, MEMBER.next)))
*/

bool is_interior(struct list_elem *elem);

void list_init(struct list *);

struct list_elem *list_begin(struct list *);
struct list_elem *list_end(struct list *);

void list_insert(struct list_elem *, struct list_elem *);
void list_push_front(struct list *, struct list_elem *);
void list_push_back(struct list *, struct list_elem *);

struct list_elem *list_remove(struct list_elem *);
struct list_elem *list_pop_front(struct list *);

struct list_elem *list_front(struct list *);

bool list_empty_(struct list *);

struct list_elem *list_last(struct list *);

void change_list(struct list *old_list, struct list *new_list); 

struct list_elem * list_before(struct list_elem *elem);
struct list_elem * list_next(struct list_elem *elem);

#endif
