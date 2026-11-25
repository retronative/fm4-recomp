// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SpeedBreaker / ReCarbon contributors
// Copyright (C) 2026 FM4 Recomp contributors
// Standalone offscreen comparison port of FM4 Recomp runtime/video/post_aa.h
// (ReCarbon 66401ea) and runtime/video/hdr_output.h (SpeedBreaker ea7311a).
// Standard AA uses the reference swap descriptors and dispatch width. HDR step
// -1 performs the encoded-to-float nearest copy before the original three steps.
constexpr char kComparisonAa[] = R"glsl(#version 450
layout(local_size_x = 16, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 1, binding = 0, rgb10_a2) uniform writeonly image2D dst;
// edgeMin: no AA on darker contrast than this; edgeRelative: ... or than this fraction of the local maximum (post_aa.h Tuning)
layout(push_constant) uniform P { ivec2 size; vec2 inverseSize; } p;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 uv) { return textureLod(src, uv, 0.0).rgb; }
vec3 texel3(ivec2 t) { return texelFetch(src, clamp(t, ivec2(0), p.size - 1), 0).rgb; }

#ifdef FM4_SHARED_LUMA
// One 16x8 workgroup shares its neighbourhood in full precision. A one-texel
// border supplies every diagonal and cardinal at the group's edges.
shared float tileLuma[10][18];
#endif

void main()
{
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
#ifdef FM4_SHARED_LUMA
    ivec2 local = ivec2(gl_LocalInvocationID.xy);
    ivec2 t = local + ivec2(1);
    vec4 center = vec4(texel3(pix), 1.0);
    float lM = luma(center.rgb);
    tileLuma[t.y][t.x] = lM;
    int lane = int(gl_LocalInvocationIndex);
    if (lane < 52) {
        // First 36 lanes load the entire top and bottom rows, including
        // corners; the next 16 load the middle cells of the two side columns.
        ivec2 halo = lane < 36 ? ivec2(lane % 18, lane < 18 ? 0 : 9)
                               : ivec2(lane < 44 ? 0 : 17, 1 + (lane - 36) % 8);
        tileLuma[halo.y][halo.x] = luma(texel3(pix - local + halo - ivec2(1)));
    }
    // All lanes, including padding in the final group, initialise and
    // synchronize before any lane can exit.
    barrier();
    if (pix.x >= p.size.x || pix.y >= p.size.y)
        return;
    float lN = tileLuma[t.y-1][t.x], lS = tileLuma[t.y+1][t.x];
    float lW = tileLuma[t.y][t.x-1], lE = tileLuma[t.y][t.x+1];
#else
    if (pix.x >= p.size.x || pix.y >= p.size.y)
        return;
    vec4 center = vec4(texelFetch(src, pix, 0).rgb, 1.0);
    float lM = luma(center.rgb);
    float lN = luma(texel3(pix + ivec2(0, -1))), lS = luma(texel3(pix + ivec2(0, 1)));
    float lW = luma(texel3(pix + ivec2(-1, 0))), lE = luma(texel3(pix + ivec2(1, 0)));
#endif
    vec2 texel = 1.0 / vec2(p.size);
    vec2 uv = (vec2(pix) + 0.5) * texel;
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125))
    {
        imageStore(dst, pix, center);
        return;
    }
#ifdef FM4_SHARED_LUMA
    float lNW = tileLuma[t.y-1][t.x-1], lSE = tileLuma[t.y+1][t.x+1];
    float lNE = tileLuma[t.y-1][t.x+1], lSW = tileLuma[t.y+1][t.x-1];
#else
    float lNW = luma(texel3(pix + ivec2(-1, -1))), lSE = luma(texel3(pix + ivec2(1, 1)));
    float lNE = luma(texel3(pix + ivec2(1, -1))), lSW = luma(texel3(pix + ivec2(-1, 1)));
#endif

    // The edge's orientation: where luma changes most (across rows for a horizontal edge).
    float acrossRows = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    float acrossColumns = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    bool horizontal = acrossRows >= acrossColumns;

    // Which side of this pixel the edge is on: the steeper neighbour.
    float lBefore = horizontal ? lN : lW, lAfter = horizontal ? lS : lE;
    float gBefore = abs(lBefore - lM), gAfter = abs(lAfter - lM);
    bool before = gBefore >= gAfter;
    float threshold = 0.25 * max(gBefore, gAfter);
    float step = horizontal ? texel.y : texel.x;
    float edgeLuma = 0.5 * ((before ? lBefore : lAfter) + lM);
    if (before)
        step = -step;

    // Walk along the edge both ways until the luma leaves the edge's.
    vec2 onEdge = uv;
    if (horizontal) onEdge.y += 0.5 * step; else onEdge.x += 0.5 * step;
    vec2 along = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uvA = onEdge - along, uvB = onEdge + along;
    float dA = luma(at(uvA)) - edgeLuma, dB = luma(at(uvB)) - edgeLuma;
    bool endA = abs(dA) >= threshold, endB = abs(dB) >= threshold;
    const float strides[10] = float[](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(endA && endB); i++)
    {
        if (!endA) { uvA -= along * strides[i]; dA = luma(at(uvA)) - edgeLuma; endA = abs(dA) >= threshold; }
        if (!endB) { uvB += along * strides[i]; dB = luma(at(uvB)) - edgeLuma; endB = abs(dB) >= threshold; }
    }
    float distA = horizontal ? uv.x - uvA.x : uv.y - uvA.y;
    float distB = horizontal ? uvB.x - uv.x : uvB.y - uv.y;
    bool nearerA = distA < distB;
    float offset = 0.5 - min(distA, distB) / (distA + distB);
    // Only blend from the end whose luma variation goes the way this pixel's does (otherwise this pixel is on the edge's far side).
    if (((nearerA ? dA : dB) < 0.0) == (lM < edgeLuma))
        offset = 0.0;

    // Sub-pixel aliasing (thin lines, single-pixel features).
    float average = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(average - lM) / range, 0.0, 1.0);
    sub = (3.0 - 2.0 * sub) * sub * sub;
    offset = max(offset, sub * sub * 0.5);
    if (offset <= 0.0)
    {
        imageStore(dst, pix, center);
        return;
    }
    vec2 sampleUv = uv;
    if (horizontal) sampleUv.y += offset * step; else sampleUv.x += offset * step;
    imageStore(dst, pix, vec4(at(sampleUv), center.a));
}
)glsl";

const char* ComparisonStandardAaSource() {
  const char* setting = std::getenv("FM4_REFERENCE_STANDARD_TILE");
  if (!setting || std::strcmp(setting, "1")) return kComparisonAa;
  constexpr char prefix[] = "#version 450\n";
  static_assert(std::string_view(kComparisonAa).starts_with(prefix));
  static const std::string tiled = std::string(prefix) + "#define FM4_SHARED_LUMA 1\n" +
                                   (kComparisonAa + sizeof(prefix) - 1);
  REXGPU_INFO("Reference Standard AA: 16x8 shared luma tile");
  return tiled.c_str();
}

constexpr char kComparisonHdr[] = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba16f) uniform image2D img;
layout(set = 0, binding = 1, std430) buffer State
{
    float average;        // the limiter's smoothed frame average (SmoothAverage)
    float framePeak;      // this frame's peak (FramePeak)
    float pad0, pad1;
    vec2 cell[576];       // per cell: mean linear luma, mean luma^8
    float field[576];     // per cell: the mean luma^8 of the 3x3 cells around it
} s;
layout(push_constant) uniform P { ivec2 size; int step; float peak; float budget; } p;
layout(set = 0, binding = 2) uniform sampler2D encoded;
const ivec2 kCells = ivec2(32, 18);

float decode(float v) { v = clamp(v, 0.0, 1.0); return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
vec3 linear(ivec2 at) { vec3 c = imageLoad(img, clamp(at, ivec2(0), p.size - 1)).rgb; return vec3(decode(c.r), decode(c.g), decode(c.b)); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float fieldAt(ivec2 c) { c = clamp(c, ivec2(0), kCells - 1); return s.field[c.y * kCells.x + c.x]; }

void main()
{
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (p.step < 0)
    {
        if (all(lessThan(id, p.size))) imageStore(img, id, texelFetch(encoded, id, 0));
        return;
    }
    if (p.step == 0)
    {
        // A cell's means from an 8x8 grid of its pixels.
        if (id.x >= kCells.x || id.y >= kCells.y)
            return;
        vec2 origin = vec2(id) * vec2(p.size) / vec2(kCells), extent = vec2(p.size) / vec2(kCells);
        float sum = 0.0, sum8 = 0.0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
            {
                float l = luma(linear(ivec2(origin + (vec2(x, y) + 0.5) * extent / 8.0)));
                float l2 = l * l, l4 = l2 * l2;
                sum += l, sum8 += l4 * l4;
            }
        s.cell[id.y * kCells.x + id.x] = vec2(sum, sum8) / 64.0;
    }
    else if (p.step == 1)
    {
        if (id != ivec2(0))
            return;
        float sum = 0.0;
        for (int i = 0; i < 576; i++)
            sum += s.cell[i].x;
        float average = clamp(sum / 576.0, 0.0, 1.0);
        float previous = clamp(s.average, 0.0, 1.0);
        average = average > previous ? average : previous + (average - previous) * 0.15;
        s.average = average;
        float base = max(p.peak, 1.0);
        s.framePeak = average <= 0.0 ? base : clamp(min(base, 1.0 / average), 1.0, base);
        for (int cy = 0; cy < kCells.y; cy++)
            for (int cx = 0; cx < kCells.x; cx++)
            {
                float v8 = 0.0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        ivec2 n = clamp(ivec2(cx + dx, cy + dy), ivec2(0), kCells - 1);
                        v8 += s.cell[n.y * kCells.x + n.x].y;
                    }
                s.field[cy * kCells.x + cx] = v8 / 9.0;
            }
    }
    else
    {
        if (id.x >= p.size.x || id.y >= p.size.y)
            return;
        vec3 c = linear(id);
        float l = luma(c);
        if (l > 0.0)
        {
            // The neighbourhood field between the four nearest cell centres.
            vec2 at = (vec2(id) + 0.5) * vec2(kCells) / vec2(p.size) - 0.5;
            ivec2 c0 = ivec2(floor(at));
            vec2 f = at - vec2(c0);
            float v8 = mix(mix(fieldAt(c0), fieldAt(c0 + ivec2(1, 0)), f.x), mix(fieldAt(c0 + ivec2(0, 1)), fieldAt(c0 + ivec2(1, 1)), f.x), f.y);
            float peak = max(s.framePeak, 1.0);
            float neighbourhood = (peak - 1.0) * max(v8, 0.0);
            float local = p.budget <= 0.0 || neighbourhood <= p.budget ? 1.0 : p.budget / neighbourhood;
            float l2 = l * l, l4 = l2 * l2;
            c *= (l + (peak - 1.0) * l4 * l4 * local) / l;
        }
        imageStore(img, id, vec4(c, 1.0));
    }
}
)glsl";

// Offline comparison only. The command processor owns these objects until its
// normal shutdown drains the queue; descriptors reuse its fenced frame slots.
bool ComparisonPostEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("FM4_BENCH_MATCH_OUTPUT");
    return value && !std::strcmp(value, "1");
  }();
  return enabled;
}

struct ComparisonGpuOutput {
  VkDescriptorSetLayout bindings{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkDescriptorPool pool{};
  std::array<VkDescriptorSet, 3> sets{};
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory image_memory{};
  VkBuffer state{};
  VkDeviceMemory state_memory{};
  uint64_t frames{};
  uint32_t last_width{}, last_height{};
};
ComparisonGpuOutput comparison_output;

void DestroyComparisonOutput(const ui::vulkan::VulkanDevice* vd) {
  auto& out = comparison_output;
  const VkDevice dev = vd->device();
  const auto& d = vd->functions();
  if (out.pipeline) d.vkDestroyPipeline(dev, out.pipeline, nullptr);
  if (out.pool) d.vkDestroyDescriptorPool(dev, out.pool, nullptr);
  if (out.layout) d.vkDestroyPipelineLayout(dev, out.layout, nullptr);
  if (out.bindings) d.vkDestroyDescriptorSetLayout(dev, out.bindings, nullptr);
  if (out.view) d.vkDestroyImageView(dev, out.view, nullptr);
  if (out.image) d.vkDestroyImage(dev, out.image, nullptr);
  if (out.image_memory) d.vkFreeMemory(dev, out.image_memory, nullptr);
  if (out.state) d.vkDestroyBuffer(dev, out.state, nullptr);
  if (out.state_memory) d.vkFreeMemory(dev, out.state_memory, nullptr);
  out = {};
}

struct ComparisonHdrPush {
  int32_t width, height, step;
  float peak, budget;
};

bool InitializeComparisonOutput(VulkanCommandProcessor& cp) {
  auto& out = comparison_output;
  const auto* vd = cp.GetVulkanDevice();
  const auto& d = vd->functions();
  const VkDevice dev = vd->device();
  const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
  std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
  std::array<VkDescriptorPoolSize, 3> sizes{};
  const auto slots = static_cast<uint32_t>(out.sets.size());
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sizes[i] = {types[i], slots};
  }
  VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout.bindingCount = 3;
  set_layout.pBindings = bindings.data();
  if (d.vkCreateDescriptorSetLayout(dev, &set_layout, nullptr, &out.bindings) != VK_SUCCESS)
    return false;
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComparisonHdrPush)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &out.bindings;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  if (d.vkCreatePipelineLayout(dev, &layout, nullptr, &out.layout) != VK_SUCCESS) return false;

  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = slots;
  pool.poolSizeCount = 3;
  pool.pPoolSizes = sizes.data();
  if (d.vkCreateDescriptorPool(dev, &pool, nullptr, &out.pool) != VK_SUCCESS) return false;
  const std::array<VkDescriptorSetLayout, 3> layouts{out.bindings, out.bindings, out.bindings};
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = out.pool;
  allocate.descriptorSetCount = slots;
  allocate.pSetLayouts = layouts.data();
  if (d.vkAllocateDescriptorSets(dev, &allocate, out.sets.data()) != VK_SUCCESS) return false;

  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image.extent = {2560, 1440, 1};
  image.mipLevels = image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vd, image, ui::vulkan::util::MemoryPurpose::kDeviceLocal, out.image, out.image_memory))
    return false;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = image.format;
  view.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (d.vkCreateImageView(dev, &view, nullptr, &out.view) != VK_SUCCESS) return false;

  constexpr VkDeviceSize state_size = 16 + 32 * 18 * 12;
  VkDeviceSize allocated = 0;
  uint32_t memory_type = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vd, state_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ui::vulkan::util::MemoryPurpose::kUpload,
          out.state, out.state_memory, &memory_type, &allocated)) return false;
  void* mapped = nullptr;
  if (d.vkMapMemory(dev, out.state_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    return false;
  std::memset(mapped, 0, state_size);
  ui::vulkan::util::FlushMappedMemoryRange(vd, out.state_memory, memory_type, 0, allocated, state_size);
  d.vkUnmapMemory(dev, out.state_memory);

  std::vector<uint32_t> spirv;
  std::string error;
  if (!cp.CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kComparisonHdr, spirv, error)) {
    REXGPU_ERROR("Matched HDR shader compile failed: {}", error);
    return false;
  }
  out.pipeline = ui::vulkan::util::CreateComputePipeline(vd, out.layout, spirv.data(),
                                                         spirv.size() * sizeof(uint32_t));
  return out.pipeline != VK_NULL_HANDLE;
}

bool RecordComparisonOutput(VulkanCommandProcessor& cp, VkImage mailbox, VkImageView mailbox_view,
                            uint32_t width, uint32_t height, uint32_t slot, VkSampler sampler) {
  auto& out = comparison_output;
  const auto* vd = cp.GetVulkanDevice();
  if (!mailbox || !mailbox_view || !sampler || !width || !height || width > 2560 || height > 1440 ||
      slot >= out.sets.size()) {
    REXGPU_ERROR("Invalid matched output target: {}x{} slot {}", width, height, slot);
    return false;
  }
  if (!out.pipeline && !InitializeComparisonOutput(cp)) {
    REXGPU_ERROR("Failed to create the benchmark HDR output");
    DestroyComparisonOutput(vd);
    return false;
  }
  const VkDescriptorImageInfo target{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo state{out.state, 0, VK_WHOLE_SIZE};
  const VkDescriptorImageInfo source{sampler, mailbox_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  std::array<VkWriteDescriptorSet, 3> writes{};
  for (uint32_t i = 0; i < writes.size(); ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = out.sets[slot];
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
  }
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[0].pImageInfo = &target;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &state;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[2].pImageInfo = &source;
  vd->functions().vkUpdateDescriptorSets(vd->device(), 3, writes.data(), 0, nullptr);

  const auto range = ui::vulkan::util::InitializeSubresourceRange();
  const auto compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  const auto shader_read = VK_ACCESS_SHADER_READ_BIT;
  const VkAccessFlags read_write = shader_read | VK_ACCESS_SHADER_WRITE_BIT;
  cp.PushImageMemoryBarrier(mailbox, range, compute | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           compute, VK_ACCESS_SHADER_WRITE_BIT | shader_read, shader_read,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  cp.PushImageMemoryBarrier(out.image, range,
                           out.frames ? compute | VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           compute, out.frames ? read_write | VK_ACCESS_TRANSFER_READ_BIT : 0,
                           VK_ACCESS_SHADER_WRITE_BIT,
                           out.frames ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  cp.PushBufferMemoryBarrier(out.state, 0, VK_WHOLE_SIZE, out.frames ? compute : VK_PIPELINE_STAGE_HOST_BIT,
                            compute, out.frames ? read_write : VK_ACCESS_HOST_WRITE_BIT, read_write,
                            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  cp.SubmitBarriers(true);
  auto& cmd = cp.deferred_command_buffer();
  cp.BindExternalComputePipeline(out.pipeline);
  cmd.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, out.layout, 0, 1, &out.sets[slot], 0, nullptr);
  ComparisonHdrPush push{int32_t(width), int32_t(height), -1, 2.0f, 0.1f};
  const uint32_t gx = (width + 7) / 8, gy = (height + 7) / 8;
  const uint32_t groups[][2] = {{gx, gy}, {4, 3}, {1, 1}, {gx, gy}};
  VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  between.srcAccessMask = read_write;
  between.dstAccessMask = read_write;
  for (unsigned i = 0; i < 4; ++i) {
    if (i) cmd.CmdVkPipelineBarrier(compute, compute, 0, 1, &between, 0, nullptr, 0, nullptr);
    push.step = int32_t(i) - 1;
    cmd.CmdVkPushConstants(out.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    cmd.CmdVkDispatch(groups[i][0], groups[i][1], 1);
  }
  cp.PushImageMemoryBarrier(mailbox, range, compute, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           shader_read, shader_read, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  cp.SubmitBarriers(true);
  ++out.frames;
  if (out.frames == 1 || out.frames % 300 == 0 || width != out.last_width || height != out.last_height)
    REXGPU_INFO("fm4bench output: frame={} {}x{} Standard AA, RGBA16F HDR headroom=2.0 budget=0.1", out.frames, width, height);
  out.last_width = width;
  out.last_height = height;
  return true;
}
