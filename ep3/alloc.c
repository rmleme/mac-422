/* This file is concerned with allocating and freeing arbitrary-size blocks of
 * physical memory on behalf of the FORK and EXEC system calls.  The key data
 * structure used is the hole table, which maintains a list of holes in memory.
 * It is kept sorted in order of increasing memory address. The addresses
 * it contains refer to physical memory, starting at absolute address 0
 * (i.e., they are not relative to the start of MM).  During system
 * initialization, that part of memory containing the interrupt vectors,
 * kernel, and MM are "allocated" to mark them as not available and to
 * remove them from the hole list.
 *
 * The entry points into this file are:
 *   alloc_mem: allocate a given sized chunk of memory
 *   free_mem:  release a previously allocated chunk of memory
 *   mem_init:  initialize the tables when MM start up
 *   max_hole:  returns the largest hole currently available
 */

#include "mm.h"
#include <minix/com.h>

/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/

#include <signal.h>
#include "mproc.h" 

/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/

#define NR_HOLES         128    /* max # entries in hole table */
#define NIL_HOLE (struct hole *) 0

PRIVATE struct hole {
  phys_clicks h_base;           /* where does the hole begin? */
  phys_clicks h_len;            /* how big is the hole? */
  struct hole *h_next;          /* pointer to next entry on the list */
} hole[NR_HOLES];


PRIVATE struct hole *hole_head; /* pointer to first hole */
PRIVATE struct hole *free_slots;        /* ptr to list of unused table slots */

FORWARD _PROTOTYPE( void del_slot, (struct hole *prev_ptr, struct hole *hp) );

/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/

FORWARD _PROTOTYPE( int merge, (struct hole *hp)                            );
FORWARD _PROTOTYPE( int move_pedaco_de_memoria, (phys_clicks origem, 
		    phys_clicks destino, phys_clicks tamanho)               );
FORWARD _PROTOTYPE( phys_clicks mem_livre, (void)                           );
FORWARD _PROTOTYPE( phys_clicks aloca_lacuna, (phys_clicks clicks)          );
FORWARD _PROTOTYPE( phys_bytes mem_usada, (void)                            );


/*===========================================================================*
 *                              alloc_mem                                    *
 *===========================================================================*/
PUBLIC phys_clicks alloc_mem(clicks)
phys_clicks clicks;             /* amount of memory requested */
{
/* Allocate a block of memory from the free list using first fit. The block
 * consists of a sequence of contiguous bytes, whose length in clicks is
 * given by 'clicks'.  A pointer to the block is returned.  The block is
 * always on a click boundary.  This procedure is called when memory is
 * needed for FORK or EXEC.
 */

  register struct mproc *proc_atual,
			*proc;
  register struct hole *hp,
		       *prev_ptr;
  phys_clicks old_base,
	      proc_seg,
	      tamanho;
  int i,
      j,
      compactou;

  if (clicks > mem_livre())   /* A memoria total disponivel nao e suficiente */
    return(NO_MEM);

  if (mem_usada() < 102400)          /* So compacta quando a porcentagem */
    return aloca_lacuna(clicks);     /* de memoria usada >= 100 Kb       */

  /* Compacta a memoria */
  hp = hole_head;
  while (hp != NIL_HOLE) {
    proc_seg = hp->h_base + hp->h_len;
    for (i = 0; i < NR_PROCS; i++) {
      proc_atual = &mproc[i];
      compactou  = 0;
      if ((proc_atual->mp_flags & IN_USE) != 0)
	if (proc_atual->mp_seg[T].mem_phys + proc_atual->mp_seg[T].mem_len ==
	    proc_atual->mp_seg[D].mem_phys) {       /* O texto do processo esta junto dos dados, de move-lo */
	  if (proc_atual->mp_seg[T].mem_phys == proc_seg) {
	    tamanho = proc_atual->mp_seg[S].mem_len -
		      proc_atual->mp_seg[D].mem_phys +
		      proc_atual->mp_seg[T].mem_len +
		      proc_atual->mp_seg[S].mem_phys;
	    if (move_pedaco_de_memoria(proc_seg, hp->h_base, tamanho) == 0)
	      break;

	    /* Atualiza na tabela de processos e no kernel */
	    /* o segmento de memoria                       */
	    proc_atual->mp_seg[S].mem_phys -= hp->h_len;
	    proc_atual->mp_seg[D].mem_phys -= hp->h_len;
	    proc_atual->mp_seg[T].mem_phys -= hp->h_len;
	    sys_newmap(i, proc_atual->mp_seg);

	    /* Atualiza lista de lacunas */
	    hp->h_base += tamanho;
	    compactou   = merge (hp);

	    /* Atualiza apontadores de texto compartilhado */
	    for (j = 0; j < NR_PROCS; j++) {
	      proc = &mproc[j];
	      if ((proc->mp_flags & IN_USE) != 0)
		if (proc->mp_seg[T].mem_phys == proc_seg) {
		  proc->mp_seg[T].mem_phys = proc_atual->mp_seg[T].mem_phys;
		  sys_newmap(j, proc->mp_seg);     /* Atualiza no kernel */
		}
	    }
	  }
	} 
	else      /* O texto do processo nao esta junto dos dados; texto compartilhado */
	  if (proc_atual->mp_seg[D].mem_phys == proc_seg) {
	    tamanho = proc_atual->mp_seg[S].mem_len -
		      proc_atual->mp_seg[D].mem_phys +
		      proc_atual->mp_seg[S].mem_phys;
	    if (move_pedaco_de_memoria(proc_seg, hp->h_base, tamanho) == 0)
	      break;

	    /* Atualiza na tabela de processos e no kernel */
	    /* o segmento de memoria                       */
	    proc_atual->mp_seg[S].mem_phys -= hp->h_len;
	    proc_atual->mp_seg[D].mem_phys -= hp->h_len;
	    sys_newmap(i, proc_atual->mp_seg);

	    /* Atualiza lista de lacunas */
	    hp->h_base += tamanho;
	    compactou   = merge (hp);
	  }
    }
    if (!compactou)
      hp = hp->h_next;
  }            

  /* Memoria ja compactada, deve procurar lacuna novamente */
  return aloca_lacuna(clicks);
}

/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/        


/*===========================================================================*
 *                              free_mem                                     *
 *===========================================================================*/
PUBLIC void free_mem(base, clicks)
phys_clicks base;               /* base address of block to free */
phys_clicks clicks;             /* number of clicks to free */
{
/* Return a block of free memory to the hole list.  The parameters tell where
 * the block starts in physical memory and how big it is.  The block is added
 * to the hole list.  If it is contiguous with an existing hole on either end,
 * it is merged with the hole or holes.
 */

  register struct hole *hp, *new_ptr, *prev_ptr;

  if (clicks == 0) return;
  if ( (new_ptr = free_slots) == NIL_HOLE) panic("Hole table full", NO_NUM);
  new_ptr->h_base = base;
  new_ptr->h_len = clicks;
  free_slots = new_ptr->h_next;
  hp = hole_head;

  /* If this block's address is numerically less than the lowest hole currently
   * available, or if no holes are currently available, put this hole on the
   * front of the hole list.
   */
  if (hp == NIL_HOLE || base <= hp->h_base) {
	/* Block to be freed goes on front of the hole list. */
	new_ptr->h_next = hp;
	hole_head = new_ptr;
	merge(new_ptr);
	return;
  }

  /* Block to be returned does not go on front of hole list. */
  while (hp != NIL_HOLE && base > hp->h_base) {
	prev_ptr = hp;
	hp = hp->h_next;
  }

  /* We found where it goes.  Insert block after 'prev_ptr'. */
  new_ptr->h_next = prev_ptr->h_next;
  prev_ptr->h_next = new_ptr;
  merge(prev_ptr);              /* sequence is 'prev_ptr', 'new_ptr', 'hp' */
}


/*===========================================================================*
 *                              del_slot                                     *
 *===========================================================================*/
PRIVATE void del_slot(prev_ptr, hp)
register struct hole *prev_ptr; /* pointer to hole entry just ahead of 'hp' */
register struct hole *hp;       /* pointer to hole entry to be removed */
{
/* Remove an entry from the hole list.  This procedure is called when a
 * request to allocate memory removes a hole in its entirety, thus reducing
 * the numbers of holes in memory, and requiring the elimination of one
 * entry in the hole list.
 */

  if (hp == hole_head)
	hole_head = hp->h_next;
  else
	prev_ptr->h_next = hp->h_next;

  hp->h_next = free_slots;
  free_slots = hp;
}


/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/

/*===========================================================================*
 *                                merge                                      *
 *===========================================================================*/
PRIVATE int merge(hp)
register struct hole *hp;       /* ptr to hole to merge with its successors */
{
/* Agora merge retorna 1 se fundiu as duas primeiras lacunas; 0 caso
 * contrario.
 */
/* Check for contiguous holes and merge any found.  Contiguous holes can occur
 * when a block of memory is freed, and it happens to abut another hole on
 * either or both ends.  The pointer 'hp' points to the first of a series of
 * three holes that can potentially all be merged together.
 */

  register struct hole *next_ptr;
  int fundiu = 0;

  /* If 'hp' points to the last hole, no merging is possible.  If it does not,
   * try to absorb its successor into it and free the successor's table entry.
   */
  if ( (next_ptr = hp->h_next) == NIL_HOLE) return 0;
  if (hp->h_base + hp->h_len == next_ptr->h_base) {
	hp->h_len += next_ptr->h_len;   /* first one gets second one's mem */
	del_slot(hp, next_ptr);
	fundiu = 1;
  } else {
	hp = next_ptr;
  }

  /* If 'hp' now points to the last hole, return; otherwise, try to absorb its
   * successor into it.
   */
  if ( (next_ptr = hp->h_next) == NIL_HOLE) return fundiu;
  if (hp->h_base + hp->h_len == next_ptr->h_base) {
	hp->h_len += next_ptr->h_len;
	del_slot(hp, next_ptr);
  }
  return fundiu;
}

/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/


/*===========================================================================*
 *                              max_hole                                     *
 *===========================================================================*/
PUBLIC phys_clicks max_hole()
{
/* Scan the hole list and return the largest hole. */

  register struct hole *hp;
  register phys_clicks max;

  hp = hole_head;
  max = 0;
  while (hp != NIL_HOLE) {
	if (hp->h_len > max) max = hp->h_len;
	hp = hp->h_next;
  }
  return(max);
}


/*===========================================================================*
 *                              mem_init                                     *
 *===========================================================================*/
PUBLIC void mem_init(total, free)
phys_clicks *total, *free;              /* memory size summaries */
{
/* Initialize hole lists.  There are two lists: 'hole_head' points to a linked
 * list of all the holes (unused memory) in the system; 'free_slots' points to
 * a linked list of table entries that are not in use.  Initially, the former
 * list has one entry for each chunk of physical memory, and the second
 * list links together the remaining table slots.  As memory becomes more
 * fragmented in the course of time (i.e., the initial big holes break up into
 * smaller holes), new table slots are needed to represent them.  These slots
 * are taken from the list headed by 'free_slots'.
 */

  register struct hole *hp;
  phys_clicks base;             /* base address of chunk */
  phys_clicks size;             /* size of chunk */
  message mess;

  /* Put all holes on the free list. */
  for (hp = &hole[0]; hp < &hole[NR_HOLES]; hp++) hp->h_next = hp + 1;
  hole[NR_HOLES-1].h_next = NIL_HOLE;
  hole_head = NIL_HOLE;
  free_slots = &hole[0];

  /* Ask the kernel for chunks of physical memory and allocate a hole for
   * each of them.  The SYS_MEM call responds with the base and size of the
   * next chunk and the total amount of memory.
   */
  *free = 0;
  for (;;) {
	mess.m_type = SYS_MEM;
	if (sendrec(SYSTASK, &mess) != OK) panic("bad SYS_MEM?", NO_NUM);
	base = mess.m1_i1;
	size = mess.m1_i2;
	if (size == 0) break;           /* no more? */

	free_mem(base, size);
	*total = mess.m1_i3;
	*free += size;
  }
}


/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/

/*===========================================================================*
 *                              aloca_lacuna                                 *
 *===========================================================================*/
PRIVATE phys_clicks aloca_lacuna(clicks)
phys_clicks clicks;             /* amount of memory requested */
{
  register struct hole *hp, *prev_ptr;
  phys_clicks old_base;

  hp = hole_head;
  while (hp != NIL_HOLE) {
	if (hp->h_len >= clicks) {
		/* We found a hole that is big enough.  Use it. */
		old_base = hp->h_base;  /* remember where it started */
		hp->h_base += clicks;   /* bite a piece off */
		hp->h_len -= clicks;    /* ditto */

		/* If hole is only partly used, reduce size and return. */
		if (hp->h_len != 0) return(old_base);

		/* The entire hole has been used up.  Manipulate free list. */
		del_slot(prev_ptr, hp);
		return(old_base);
	}

	prev_ptr = hp;
	hp = hp->h_next;
  }
  return(NO_MEM);
}


/*===========================================================================*
 *                               mem_livre                                   *
 *===========================================================================*/
PRIVATE phys_clicks mem_livre(void)
{
/* Funcao que devolve o total de memoria livre do sistema. */

  struct hole *hp;
  phys_clicks qtde_mem = 0;

  for (hp = hole_head; hp != NIL_HOLE; hp = hp->h_next)
    qtde_mem += hp->h_len;
  return qtde_mem;
}

/*===========================================================================*
 *                               mem_usada                                   *
 *===========================================================================*/
PRIVATE phys_bytes mem_usada(void)
{
/* Funcao que devolve em bytes o total de memoria usada do sistema. */

  phys_clicks qtde_mem_usada = 0;
  register struct mproc *proc;
  int i;
 
  for (i = 0; i < NR_PROCS; i++)
  {
    proc = &mproc[i];
    if ((proc->mp_flags & IN_USE) != 0)
      qtde_mem_usada += proc->mp_seg[S].mem_len - proc->mp_seg[D].mem_phys +
			proc->mp_seg[T].mem_len + proc->mp_seg[S].mem_phys;

  }

  return (phys_bytes) qtde_mem_usada << CLICK_SHIFT;
}

/*===========================================================================*
 *                              mostra_lacunas                               *
 *===========================================================================*/
PUBLIC int mostra_lacunas(void)
{
/* Imprime na tela o endereco e o tamanho de cada lacuna da memoria. */

  struct hole *lac;

  printf("\n\nLACUNAS NA MEMORIA:\n");
  for (lac = hole_head; lac != NIL_HOLE; lac = lac->h_next)
    printf("Endereco: %0x, Tamanho: %0x\n", lac->h_base, lac->h_len);
  printf("Aperte enter.\n\n");
  dont_reply = TRUE;
  return OK;
}


/*===========================================================================*
 *                          move_pedaco_de_memoria                           *
 *===========================================================================*/
PRIVATE int move_pedaco_de_memoria(origem, destino, tamanho)
phys_clicks origem, destino, tamanho;
{
/* Funcao que move o pedaco da memoria que comeca em origem, e tem tamanho
 * tamanho, para destino. ALERTA: quem chama esta funcao deve se certificar que
 * moveu todas as referencias para a area antiga de memoria. E tambem deve
 * estar ciente de que o conteudo da memoria em destino sera apagado.
 */

  int i;
  phys_bytes orig_bytes,
	     dest_bytes,
	     tam_bytes;
  phys_clicks dif = origem - destino;

  if (dif < 0) return 0;
  if (dif == 0) return 1;

  while (tamanho > dif) {
    orig_bytes = (phys_bytes) origem << CLICK_SHIFT;
    dest_bytes = (phys_bytes) destino << CLICK_SHIFT;
    tam_bytes  = (phys_bytes) dif << CLICK_SHIFT;
    i          = sys_copy(ABS, 0, orig_bytes, ABS, 0, dest_bytes, tam_bytes);
    if (i < 0) {
      panic("Erro de movimentacao de memoria", i);
      return 0;
    }
    tamanho -= dif;
    origem  += dif;
    destino += dif;
  }

  /* Agora a "intersecao" e nula. */
  orig_bytes = (phys_bytes) origem << CLICK_SHIFT;
  dest_bytes = (phys_bytes) destino << CLICK_SHIFT;
  tam_bytes  = (phys_bytes) tamanho << CLICK_SHIFT;
  i          = sys_copy(ABS, 0, orig_bytes, ABS, 0, dest_bytes, tam_bytes);
  if (i < 0) {
    panic("Erro de movimentacao de memoria", i);
    return 0;
  }
  return 1;
}

/*???????????????????????????????????????????????????????????????????????????*/
/*???????????????????????????????????????????????????????????????????????????*/
