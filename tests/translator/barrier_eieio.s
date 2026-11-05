# eieio alone (FM4 Recomp; patch 0014): one barrier per file, so the EMITS check ties the fence to this
# instruction (ForzaRecomp's memory_barriers.s checks all three fences in one file, so swapping two
# instructions' fences would pass it). Mapping (PowerPC ISA vs C++ fences): sync orders everything
# -> seq_cst; lwsync orders all but store-load -> acq_rel; eieio orders stores -> release (stronger).
#! EMITS __atomic_thread_fence(__ATOMIC_RELEASE)

test_eieio_alone:
  #_ REGISTER_IN r3 0x3000
  li r4, 7
  stw r4, 0(r3)
  eieio
  lwz r5, 0(r3)
  blr
  #_ REGISTER_OUT r5 7
