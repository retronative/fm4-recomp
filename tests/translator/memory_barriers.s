# sync, lwsync and eieio (patches/xenonrecomp/0014) emit host fences: the PowerPC ordering they
# request (sync: all accesses; lwsync: all but store-load; eieio: stores to cacheable memory) maps
# to sequentially consistent, acquire-release and release fences. A single-threaded run cannot
# observe ordering, so the generated code is checked; the test confirms execution continues.
#! EMITS __atomic_thread_fence(__ATOMIC_SEQ_CST);
#! EMITS __atomic_thread_fence(__ATOMIC_ACQ_REL);
#! EMITS __atomic_thread_fence(__ATOMIC_RELEASE);

test_barriers_continue:
  #_ REGISTER_IN r4 0x5000
  li r3, 7
  stw r3, 0(r4)
  sync
  lwsync
  eieio
  lwz r5, 0(r4)
  blr
  #_ REGISTER_OUT r5 7
