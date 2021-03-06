//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the allocator as indicated in the handout.
// 
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include "MyMalloc.h"

#define ALLOCATED 1
#define NOT_ALLOCATED 0
#define ARENA_SIZE 2097152

pthread_mutex_t mutex;

static bool verbose = false;

extern void atExitHandlerInC()
{
  if (verbose)
    print();
}

static void * getMemoryFromOS(size_t size)
{
  // Use sbrk() to get memory from OS
  _heapSize += size;
 
  void *mem = sbrk(size);

  if(!_initialized){
      _memStart = mem;
  }

  return mem;
}


/*
 * @brief retrieves a new 2MB chunk of memory from the OS
 * and adds "dummy" boundary tags
 * @param size of the request
 * @return a FreeObject pointer to the beginning of the chunk
 */
static FreeObject * getNewChunk(size_t size)
{
  void *mem = getMemoryFromOS(size);

  // establish fence posts
  BoundaryTag *fencePostHead = (BoundaryTag *)mem;
  setAllocated(fencePostHead, ALLOCATED);
  setSize(fencePostHead, 0);

  char *temp = (char *)mem + size - sizeof(BoundaryTag);
  BoundaryTag *fencePostFoot = (BoundaryTag *)temp;
  setAllocated(fencePostFoot, ALLOCATED);
  setSize(fencePostFoot, 0);
 
  return (FreeObject *)((char *)mem + sizeof(BoundaryTag));
}

/**
 * @brief If no blocks have been allocated, get more memory and 
 * set up the free list
 */
static void initialize()
{
  verbose = true;

  pthread_mutex_init(&mutex, NULL);

  // print statistics at exit
  atexit(atExitHandlerInC);

  FreeObject *firstChunk = getNewChunk(ARENA_SIZE);

  // initialize the list to point to the firstChunk
  _freeList = &_freeListSentinel;
  setSize(&firstChunk->boundary_tag, ARENA_SIZE - (2*sizeof(BoundaryTag))); // ~2MB
  firstChunk->boundary_tag._leftObjectSize = 0;
  setAllocated(&firstChunk->boundary_tag, NOT_ALLOCATED);

  // link list pointer hookups
  firstChunk->free_list_node._next = _freeList;
  firstChunk->free_list_node._prev = _freeList;
  _freeList->free_list_node._prev = firstChunk;
  _freeList->free_list_node._next = firstChunk;

  _initialized = 1;
}

/**
 * @brief TODO: PART 1
 * This function should perform allocation to the program appropriately,
 * giving pieces of memory that large enough to satisfy the request. 
 * Currently, it just sbrk's memory every time.
 *
 * @param size size of the request
 *
 * @return pointer to the first usable byte in memory for the requesting
 * program
 */
static void * allocateObject(size_t size)
{
  // Make sure that allocator is initialized
  if (!_initialized)
    initialize();
  
  if (size == 0)
  {
    pthread_mutex_unlock(&mutex);
    return NULL;
  }
  size_t obj_size = size;
  size_t real_size = size;
  // Round up requested size to next 8 byte
  if (size % 8 == 0)
  {
    obj_size = size;
  }
  else
  {
    obj_size = size + (8 - (size % 8));
  }
  
  real_size = obj_size + sizeof(BoundaryTag);

  if(real_size > ARENA_SIZE - (3*sizeof(BoundaryTag)))
  {
    errno = ENOMEM;
    //pthread_mutex_unlock(&mutex);
    return NULL;
  }

  // Add size of the block's header

  // Min alloc size
  if(real_size < sizeof(FreeObject))
  {
    real_size = sizeof(FreeObject);
  }
  FreeObject * curr = _freeList->free_list_node._next;
  while (curr != &_freeListSentinel)
  {
    if (getSize(&(curr->boundary_tag)) >= real_size)
    {
      if (getSize(&(curr->boundary_tag)) - real_size >= sizeof(FreeObject))
      {
        // Calculate the first memory position
        void * new_obj = ((char *)curr) + (getSize(&(curr->boundary_tag)) - real_size);
        setSize((BoundaryTag *) new_obj, real_size);
        setAllocated((BoundaryTag *) new_obj, ALLOCATED);
        
        // set left obj for new_obj
        ((BoundaryTag *) new_obj)->_leftObjectSize = getSize(&(curr->boundary_tag)) - real_size;
        
        //set left obj size for entire block?
        BoundaryTag *left_obj = (BoundaryTag *)(((char *) new_obj) + getSize((BoundaryTag *)new_obj));
        left_obj->_leftObjectSize = getSize((BoundaryTag *)new_obj);
        
        // Modify free block header
        setSize((BoundaryTag *) &curr->boundary_tag, (getSize(&(curr->boundary_tag)) - real_size));
        pthread_mutex_unlock(&mutex);
        return ((char *)new_obj) + sizeof(BoundaryTag);
      }
      else {
        setAllocated((BoundaryTag *) &curr->boundary_tag, NOT_ALLOCATED);
        curr->free_list_node._prev->free_list_node._next = curr->free_list_node._next;
        curr->free_list_node._next->free_list_node._prev = curr->free_list_node._prev;
        pthread_mutex_unlock(&mutex);
        return ((char *)curr) + sizeof(BoundaryTag);
      }
    }
    else
    {
      curr = curr->free_list_node._next;
    }
  }
  FreeObject *newChunk = getNewChunk(ARENA_SIZE);
  setSize(&newChunk->boundary_tag, ARENA_SIZE - (2*sizeof(BoundaryTag)));
  newChunk->boundary_tag._leftObjectSize = 0;
  setAllocated(&newChunk->boundary_tag, NOT_ALLOCATED);
  newChunk->free_list_node._next = _freeList->free_list_node._next;
  newChunk->free_list_node._prev = _freeList;
  _freeList->free_list_node._next->free_list_node._prev = newChunk;
  _freeList->free_list_node._next = newChunk;
 
  pthread_mutex_unlock(&mutex);
  return allocateObject(size);
}

/**
 * @brief TODO: PART 2
 * This funtion takes a pointer to memory returned by the program, and
 * appropriately reinserts it back into the free list.
 * You will have to manage all coalescing as needed
 *
 * @param ptr
 */
static void freeObject(void *ptr)
{
  
  BoundaryTag *obj_head = (BoundaryTag *)(((char *)ptr) - sizeof(BoundaryTag));
  BoundaryTag *next_head = (BoundaryTag *)(((char *)obj_head) + getSize(obj_head));
  
  FreeObject *newfree = (FreeObject *) obj_head;
  
  //check if next block is free
  if (!isAllocated(next_head))
  {
    // coalese next block into curr
    setSize(obj_head, getSize(obj_head) + getSize(next_head));
    
    BoundaryTag *modnext_head = (BoundaryTag*)(((char *)obj_head) + getSize(obj_head));
    // change size of next next
    modnext_head->_leftObjectSize = getSize(obj_head);

    // remove next block from free list
    FreeObject *next = (FreeObject *) next_head;
    next->free_list_node._prev->free_list_node._next = next->free_list_node._next;
    next->free_list_node._next->free_list_node._prev = next->free_list_node._prev;
  }
  // calculate prev position

  BoundaryTag *prev_head = (BoundaryTag *)(((char *)obj_head) - obj_head->_leftObjectSize);
  //check if prev block is free
  if (!isAllocated(prev_head))
  {
    // coalese curr into prev
    setSize(prev_head, getSize(prev_head) + getSize(obj_head));
    
    BoundaryTag *modprev_head = (BoundaryTag *)(((char *)prev_head) + getSize(prev_head)); 
    
    // modify left object size for prev coalese
    modprev_head->_leftObjectSize = getSize(prev_head);
    
    pthread_mutex_unlock(&mutex);
    return;  
  } 
 
  
  setAllocated(&(newfree->boundary_tag), NOT_ALLOCATED);
  

  // modify left object size for just new free
  BoundaryTag *left_obj = (BoundaryTag *)(((char *)obj_head) + getSize(obj_head));
  left_obj->_leftObjectSize = getSize(obj_head);


  // add newly freed to free list
  newfree->free_list_node._next = _freeList->free_list_node._next;
  newfree->free_list_node._prev = _freeList;
  _freeList->free_list_node._next->free_list_node._prev = newfree;
  _freeList->free_list_node._next = newfree;

  pthread_mutex_unlock(&mutex);
  return;
}

void print()
{
  printf("\n-------------------\n");

  printf("HeapSize:\t%zd bytes\n", _heapSize);
  printf("# mallocs:\t%d\n", _mallocCalls);
  printf("# reallocs:\t%d\n", _reallocCalls);
  printf("# callocs:\t%d\n", _callocCalls);
  printf("# frees:\t%d\n", _freeCalls);

  printf("\n-------------------\n");
}

void print_list()
{
    printf("FreeList: ");
    if (!_initialized) 
        initialize();
    FreeObject *ptr = _freeList->free_list_node._next;
    while (ptr != _freeList) {
        long offset = (long)ptr - (long)_memStart;
        printf("[offset:%ld,size:%zd]", offset, getSize(&ptr->boundary_tag));
        ptr = ptr->free_list_node._next;
        if (ptr != NULL)
            printf("->");
    }
    printf("\n");
}

void increaseMallocCalls() { _mallocCalls++; }

void increaseReallocCalls() { _reallocCalls++; }

void increaseCallocCalls() { _callocCalls++; }

void increaseFreeCalls() { _freeCalls++; }

//
// C interface
//

extern void * malloc(size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseMallocCalls();
  
  void * addr =  allocateObject(size);
  //pthread_mutex_unlock(&mutex);
  return addr;
}

extern void free(void *ptr)
{
  pthread_mutex_lock(&mutex);
  increaseFreeCalls();
  
  if (ptr == 0) {
    // No object to free
    pthread_mutex_unlock(&mutex);
    return;
  }
  
  freeObject(ptr);
}

extern void * realloc(void *ptr, size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseReallocCalls();

  // Allocate new object
  void *newptr = allocateObject(size);

  // Copy old object only if ptr != 0
  if (ptr != 0) {

    // copy only the minimum number of bytes
    FreeObject *o = (FreeObject *)((char *) ptr - sizeof(BoundaryTag));
    size_t sizeToCopy = getSize(&o->boundary_tag);
    if (sizeToCopy > size) {
      sizeToCopy = size;
    }

    memcpy(newptr, ptr, sizeToCopy);

    //Free old object
    freeObject(ptr);
  }

  return newptr;
}

extern void * calloc(size_t nelem, size_t elsize)
{
  pthread_mutex_lock(&mutex);
  increaseCallocCalls();
    
  // calloc allocates and initializes
  size_t size = nelem * elsize;

  void *ptr = allocateObject(size);

  if (ptr) {
    // No error
    // Initialize chunk with 0s
    memset(ptr, 0, size);
  }

  return ptr;
}

