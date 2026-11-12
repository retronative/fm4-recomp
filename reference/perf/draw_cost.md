# CP cost per draw: FM4 vs SpeedBreaker

Read-only source study of our command processor and renderer against SpeedBreaker (another Xbox 360 port; its
renderer file:line below) and a reference Xbox 360 port with the same patterns.

## 0. The listed candidates do not close the gap
- OBSERVED at swap 6000 of one run: cp 14.8 ms for ~2170 draws; texture binds 0.8 ms (~0.37 us/draw); 2152 pushed
  sets; constants reused on 256 draws (12 %); variant memo 2170 hits. An earlier profile (older stack): Draw 61 %,
  Present 12.6 %, RetireSlot 9.5 %.
- The five listed candidates are worth ~1-1.5 us/draw (HYPOTHESIS) of a ~6 us gap. Unmeasured: MoltenVK cost per vkCmd call,
  AcquireTargets, the vertex guard path, packet-loop overhead. First step: per-phase timers in Draw + register-write and packet
  counters in the swap line.

## 1. Side by side (VERIFIED)
- Register writes: FM4 per word Reader::Next (% size + Load32 swap, command_processor.cpp:61-70) then CpWriteRegister checks
  (:244-259); type-0 one word at a time (:287-292); SET_CONSTANT (:625-633); LOAD_ALU PhysicalToGuest per word (:634-643).
  SpeedBreaker: runs without special registers in one memcpy (BulkSafe :281-288, WriteRegistersBulk :293-307, Take :345-355,
  type-0 :1086-1099, SET/LOAD constant :991-1007); the reference port does the same.
- Per packet: FM4 HonourGpuReadGuard() before every packet (:270: TakeRequest + ReapRetired, renderer.cpp:1637-1655);
  SpeedBreaker nothing.
- Register reads: FM4 DecodeDrawState ~60 Reg() through in.read (draw_state.cpp:37; renderer.cpp:1996), std::vector<Use> per
  draw (:477). SpeedBreaker inline host-order array (:8982-9070).
- Constants: FM4 always swap + memcmp vs previous (renderer.cpp:3224-3244), fill only when different (draw_record.cpp:337-348).
  SpeedBreaker upload only when dirty, packed to used constants (:9551-9590).
- Pipeline: FM4 variant memo (renderer.cpp:2755-2800), KeyFromDrawState (:2812), PlanDescriptors (:2818), PipelineCache::Get
  unordered_map every draw (pipeline_cache.cpp:265-268), byte-wise FNV over 96 bytes (pipeline_key.h:130-136). SpeedBreaker
  80-byte key, word hash (:2839-2865), last-key fast path (:3107-3120; the reference port too).
- Textures: FM4 three passes per texture per draw (readsPendingWrite :2843, stagingBytes :3031, bind :3418), each a Prepare memo
  scan (texture_upload.cpp:36-55); bind does FormatSupported, map finds, WatchClean, model.Loaded even when clean
  (texture_vk.cpp:682-808; texture_cache.h:106-125, :405-418). SpeedBreaker per-slot entry: 24-byte memcmp + epoch
  (:4905-5056; the reference port too).
- Descriptors: FM4 pushes 6 buffers + textures every draw (renderer.cpp:3447-3480, :3578); offsets change per draw (:3208-3356).
  SpeedBreaker set 0 dynamic offset bound when changed (:10679-10697), set 1 pushed only when views/samplers change
  (:10699-10730).
- Dynamic state: FM4 13-14 vkCmd calls every draw (renderer.cpp:3577-3631). SpeedBreaker BoundState skips unchanged values;
  epoch reset (:10593-10690; the reference port too).
- Vertex: FM4 per window PhysicalToGuest x2, LookupHeap, Heap::IsCommitted under mutex with a page walk (renderer.cpp:2850-2865,
  virtual_memory.cpp:299-308), GuestWritesOverlap, vertexWatches find, NoteReadThen twice (:2976-3011, :3263-3280).

## 2. Ranked changes (HYPOTHESIS until timed; each opt-in with a =2 verify mode)
- R1 BoundState: skip unchanged vkCmd state calls; reset at OpenSlot, after unit work, at each render pass begin. 0.5-2 us/draw
  (an earlier measurement found no gain with the validation layer on at 28 us/draw). Pure-model test Decide(prev, next, epoch) -> call mask.
- R2 Bulk register runs: one memcpy per qualifying run in type-0, SET_CONSTANT, SET_CONSTANT2, LOAD_ALU (both sides big-endian
  guest memory, no swap). Exclusions: >= kRegisterCount, kCoherStatusHost, kWaitUntil, kScratchReg0..+7, gamma state registers;
  no ring wrap; not repeat-one-register. 0.3-1 us/draw.
- R3 Per-slot texture bind cache keyed on fetch words, ShaderFetch, gamma, watch Epoch, pending-log generation, resident flag,
  image-map generation; =2 verify. 0.3-0.45 us/draw.
- R4 Push descriptors only when changed: simple filter refuted (offsets change per draw); restructure (dynamic set 0 + texture
  push set) 0.2-0.4 us/draw, risks recreating all pipelines.
- R5 Pipeline last-key fast path + word hash: 0.05-0.15 us/draw.
- R6 HonourGpuReadGuard only before type-3 packets: 0.05-0.2 us/draw.
- R7 DecodeDrawState direct register reads, fixed uses array; vertex epoch fast path; memoized IsCommitted: 0.1-0.2 us/draw.
- R8 Constant dirty tracking: refuted (12 % reuse).
- R9 Lazy KeyName in ClassifyTransfer (edram_vk_plan.cpp:81; DiagConsume string edram_vk.cpp:1582): ~0.02-0.05 us/draw.
Order: timers -> R1 -> R2 -> R3 -> R4 if descriptors > 0.3 us -> R5, R7, R9 -> not R8.
