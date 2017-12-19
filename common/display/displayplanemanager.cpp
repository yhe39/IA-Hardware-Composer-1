/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "displayplanemanager.h"

#include "displayplane.h"
#include "factory.h"
#include "hwctrace.h"
#include "nativesurface.h"
#include "overlaylayer.h"

namespace hwcomposer {

DisplayPlaneManager::DisplayPlaneManager(int gpu_fd,
                                         DisplayPlaneHandler *plane_handler,
                                         ResourceManager *resource_manager)
    : plane_handler_(plane_handler),
      resource_manager_(resource_manager),
      width_(0),
      height_(0),
      gpu_fd_(gpu_fd) {
}

DisplayPlaneManager::~DisplayPlaneManager() {
}

bool DisplayPlaneManager::Initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  bool status = plane_handler_->PopulatePlanes(overlay_planes_);
  if (!overlay_planes_.empty()) {
    if (overlay_planes_.size() > 1) {
      cursor_plane_ = overlay_planes_.back().get();
      bool needs_cursor_wa = false;
#ifdef DISABLE_CURSOR_PLANE
      needs_cursor_wa = overlay_planes_.size() > 3;
#endif
      // If this is a universal plane, let's not restrict it to
      // cursor usage only.
      if (!needs_cursor_wa && cursor_plane_->IsUniversal()) {
        cursor_plane_ = NULL;
      }
    }

    primary_plane_ = overlay_planes_.at(0).get();
  }

  return status;
}

bool DisplayPlaneManager::ValidateLayers(
    std::vector<OverlayLayer> &layers,
    std::vector<OverlayLayer *> &cursor_layers, bool pending_modeset,
    bool disable_overlay, DisplayPlaneStateList &composition,
    bool request_video_effect) {
  CTRACE();
  // Let's mark all planes as free to be used.
  for (auto j = overlay_planes_.begin(); j != overlay_planes_.end(); ++j) {
    j->get()->SetInUse(false);
  }

  std::vector<OverlayPlane> commit_planes;
  auto layer_begin = layers.begin();
  auto layer_end = layers.end();
  bool render_layers = false;
  // We start off with Primary plane.
  DisplayPlane *current_plane = primary_plane_;
  OverlayLayer *primary_layer = &(*(layers.begin()));
  commit_planes.emplace_back(OverlayPlane(current_plane, primary_layer));
  composition.emplace_back(current_plane, primary_layer,
                           primary_layer->GetZorder());
  current_plane->SetInUse(true);
  ++layer_begin;
  // Lets ensure we fall back to GPU composition in case
  // primary layer cannot be scanned out directly.
  bool prefer_seperate_plane = primary_layer->PreferSeparatePlane();
  bool force_gpu = (pending_modeset && layers.size() > 1) || disable_overlay;
  bool force_va = false;

  // If request video effect we have to use VA to process the video layer
  if (request_video_effect && primary_layer->IsVideoLayer()) {
    force_va = true;
  }

  if (force_gpu || force_va ||
      FallbacktoGPU(current_plane, primary_layer, commit_planes)) {
    render_layers = true;
    if (force_gpu || !prefer_seperate_plane) {
      DisplayPlaneState &last_plane = composition.back();
      for (auto i = layer_begin; i != layer_end; ++i) {
        last_plane.AddLayer(&(*(i)));
        i->GPURendered();
      }

      ResetPlaneTarget(last_plane, commit_planes.back());
      // We need to composite primary using GPU, lets use this for
      // all layers in this case.
      return render_layers;
    } else {
      DisplayPlaneState &last_plane = composition.back();
      if (primary_layer->IsVideoLayer()) {
        last_plane.SetVideoPlane();
      }

      ResetPlaneTarget(last_plane, commit_planes.back());
    }
  }

  if (render_layers)
    ValidateForDisplayScaling(composition.back(), commit_planes, primary_layer);

  // We are just compositing Primary layer and nothing else.
  if (layers.size() == 1) {
    return render_layers;
  }

  if (layer_begin != layer_end) {
    // Handle layers for overlay
    uint32_t index = 0;
    for (auto j = overlay_planes_.begin() + 1; j != overlay_planes_.end();
         ++j) {
#ifdef DISABLE_CURSOR_PLANE
      if (cursor_plane_ == j->get())
        continue;
#endif
      DisplayPlaneState &last_plane = composition.back();
      OverlayLayer *previous_layer = NULL;
      if (previous_layer && last_plane.NeedsOffScreenComposition()) {
        ValidateForDisplayScaling(composition.back(), commit_planes,
                                  previous_layer);
        render_layers = true;
      }

      // Handle remaining overlay planes.
      for (auto i = layer_begin; i != layer_end; ++i) {
        OverlayLayer *layer = &(*(i));
        commit_planes.emplace_back(OverlayPlane(j->get(), layer));
        index = i->GetZorder();
        ++layer_begin;
        // If we are able to composite buffer with the given plane, lets use
        // it.
        bool fall_back = FallbacktoGPU(j->get(), layer, commit_planes);
        if (request_video_effect && layer->IsVideoLayer()) {
          fall_back = true;
        }

        if (!fall_back || prefer_seperate_plane ||
            layer->PreferSeparatePlane()) {
          composition.emplace_back(j->get(), layer, index);
          j->get()->SetInUse(true);
          if (fall_back) {
            DisplayPlaneState &last_plane = composition.back();
            if (layer->IsVideoLayer()) {
              last_plane.SetVideoPlane();
            }

            ResetPlaneTarget(last_plane, commit_planes.back());
          }

          prefer_seperate_plane = layer->PreferSeparatePlane();
          break;
        } else {
          last_plane.AddLayer(layer);
          if (!last_plane.GetOffScreenTarget()) {
            SetOffScreenPlaneTarget(last_plane);
          }

          commit_planes.pop_back();
        }

        previous_layer = layer;
      }
    }

    DisplayPlaneState &last_plane = composition.back();
    bool is_video = last_plane.IsVideoPlane();
    OverlayLayer *previous_layer = NULL;
    // We dont have any additional planes. Pre composite remaining layers
    // to the last overlay plane.
    for (auto i = layer_begin; i != layer_end; ++i) {
      previous_layer = &(*(i));
      last_plane.AddLayer(previous_layer);
    }

    if (last_plane.NeedsOffScreenComposition()) {
      if (previous_layer) {
        // In this case we need to fallback to 3Dcomposition till Media
        // backend adds support for multiple layers.
        bool force_buffer = false;
        if (is_video && last_plane.source_layers().size() > 1 &&
            last_plane.GetOffScreenTarget()) {
          last_plane.GetOffScreenTarget()->SetInUse(false);
          std::vector<NativeSurface *>().swap(last_plane.GetSurfaces());
          force_buffer = true;
        }

        if (!last_plane.GetOffScreenTarget() || force_buffer) {
          ResetPlaneTarget(last_plane, commit_planes.back());
        }

        ValidateForDisplayScaling(composition.back(), commit_planes,
                                  previous_layer);
      }

      render_layers = true;
    }
  }

  bool render_cursor_layer = ValidateCursorLayer(cursor_layers, composition);
  if (!render_layers) {
    render_layers = render_cursor_layer;
  }

  if (render_layers) {
    ValidateFinalLayers(composition, layers);
    for (DisplayPlaneState &plane : composition) {
      if (plane.NeedsOffScreenComposition()) {
        const std::vector<size_t> &source_layers = plane.source_layers();
        size_t layers_size = source_layers.size();
        bool useplanescalar = plane.IsUsingPlaneScalar();
        for (size_t i = 0; i < layers_size; i++) {
          size_t source_index = source_layers.at(i);
          OverlayLayer &layer = layers.at(source_index);
          layer.GPURendered();
          layer.UsePlaneScalar(useplanescalar);
        }
      }
    }
  }

  return render_layers;
}

bool DisplayPlaneManager::ReValidateLayers(std::vector<OverlayLayer> &layers,
                                           DisplayPlaneStateList &composition,
                                           bool *request_full_validation) {
  CTRACE();
  std::vector<OverlayPlane> commit_planes;
  for (DisplayPlaneState &temp : composition) {
    commit_planes.emplace_back(
        OverlayPlane(temp.plane(), temp.GetOverlayLayer()));
    // Check if we can still need/use scalar for this plane.
    if (temp.IsUsingPlaneScalar()) {
      const std::vector<size_t> &source = temp.source_layers();
      size_t total_layers = source.size();
      ValidateForDisplayScaling(
          temp, commit_planes, &(layers.at(source.at(total_layers - 1))), true);
    }
  }

  bool render_layers = false;
  // If this combination fails just fall back to 3D for all layers.
  if (plane_handler_->TestCommit(commit_planes)) {
    *request_full_validation = false;
    for (DisplayPlaneState &plane : composition) {
      if (plane.NeedsOffScreenComposition()) {
        render_layers = true;
        const std::vector<size_t> &source_layers = plane.source_layers();
        size_t layers_size = source_layers.size();
        bool useplanescalar = plane.IsUsingPlaneScalar();
        for (size_t i = 0; i < layers_size; i++) {
          size_t source_index = source_layers.at(i);
          OverlayLayer &layer = layers.at(source_index);
          layer.GPURendered();
          layer.UsePlaneScalar(useplanescalar);
        }
      }
    }
  } else {
    *request_full_validation = true;
  }

  return render_layers;
}

DisplayPlaneState *DisplayPlaneManager::GetLastUsedOverlay(
    DisplayPlaneStateList &composition) {
  CTRACE();

  DisplayPlaneState *last_plane = NULL;
  size_t size = composition.size();
  for (size_t i = size; i > 0; i--) {
    DisplayPlaneState &plane = composition.at(i - 1);
    if ((cursor_plane_ == plane.plane()) && (!cursor_plane_->IsUniversal()))
      continue;

    last_plane = &plane;
    break;
  }

  return last_plane;
}

void DisplayPlaneManager::PreparePlaneForCursor(DisplayPlaneState *plane,
                                                bool reset_buffer) {
  NativeSurface *surface = plane->GetOffScreenTarget();
  if (surface && reset_buffer) {
    surface->SetInUse(false);
  }

  if (!surface || reset_buffer) {
    SetOffScreenPlaneTarget(*plane);
  }

  std::vector<CompositionRegion> &comp_regions = plane->GetCompositionRegion();
  std::vector<CompositionRegion>().swap(comp_regions);
  std::vector<NativeSurface *> &surfaces = plane->GetSurfaces();
  size_t size = surfaces.size();
  const HwcRect<int> &current_rect = plane->GetDisplayFrame();
  for (size_t i = 0; i < size; i++) {
    surfaces.at(i)->ResetDisplayFrame(current_rect);
  }

  plane->SwapSurfaceIfNeeded();
}

bool DisplayPlaneManager::ValidateCursorLayer(
    std::vector<OverlayLayer *> &cursor_layers,
    DisplayPlaneStateList &composition) {
  CTRACE();
  if (cursor_layers.empty()) {
    return false;
  }

  std::vector<OverlayPlane> commit_planes;
  DisplayPlaneState *last_plane = GetLastUsedOverlay(composition);
  bool is_video = last_plane->IsVideoPlane();
  for (DisplayPlaneState &temp : composition) {
    commit_planes.emplace_back(
        OverlayPlane(temp.plane(), temp.GetOverlayLayer()));
  }

  uint32_t total_size = cursor_layers.size();
  bool status = false;
  uint32_t cursor_index = 0;
  for (auto j = overlay_planes_.rbegin(); j != overlay_planes_.rend(); ++j) {
    if (cursor_index == total_size)
      break;

    DisplayPlane *plane = j->get();
    if (plane->InUse())
      break;

#ifdef DISABLE_CURSOR_PLANE
    if (cursor_plane_ == plane)
      continue;
#endif
    OverlayLayer *cursor_layer = cursor_layers.at(cursor_index);
    commit_planes.emplace_back(OverlayPlane(plane, cursor_layer));
    // Lets ensure we fall back to GPU composition in case
    // cursor layer cannot be scanned out directly.
    if (FallbacktoGPU(plane, cursor_layer, commit_planes)) {
      commit_planes.pop_back();
      cursor_layer->GPURendered();
      last_plane->AddLayer(cursor_layer);
      bool reset_overlay = false;
      if (!last_plane->GetOffScreenTarget() || is_video)
        reset_overlay = true;

      PreparePlaneForCursor(last_plane, is_video);

      if (reset_overlay) {
        // Layer for the plane should have changed, reset commit planes.
        std::vector<OverlayPlane>().swap(commit_planes);
        for (DisplayPlaneState &temp : composition) {
          commit_planes.emplace_back(
              OverlayPlane(temp.plane(), temp.GetOverlayLayer()));
        }
      }

      ValidateForDisplayScaling(*last_plane, commit_planes, cursor_layer,
                                false);
      status = true;
    } else {
      composition.emplace_back(plane, cursor_layer, cursor_layer->GetZorder());
      plane->SetInUse(true);
      last_plane = GetLastUsedOverlay(composition);
      is_video = last_plane->IsVideoPlane();
    }

    cursor_index++;
  }

  // We dont have any additional planes. Pre composite remaining cursor layers
  // to the last overlay plane.
  OverlayLayer *last_layer = NULL;
  for (uint32_t i = cursor_index; i < total_size; i++) {
    OverlayLayer *cursor_layer = cursor_layers.at(i);
    last_plane->AddLayer(cursor_layer);
    cursor_layer->GPURendered();
    status = true;
    last_layer = cursor_layer;
  }

  if (last_layer) {
    PreparePlaneForCursor(last_plane, is_video);
    ValidateForDisplayScaling(*last_plane, commit_planes, last_layer, false);
  }

  return status;
}

void DisplayPlaneManager::ValidateForDisplayScaling(
    DisplayPlaneState &last_plane, std::vector<OverlayPlane> &commit_planes,
    OverlayLayer *current_layer, bool ignore_format) {
  size_t total_layers = last_plane.source_layers().size();
  std::vector<NativeSurface *> &surfaces = last_plane.GetSurfaces();
  size_t size = surfaces.size();

  if (last_plane.IsUsingPlaneScalar()) {
    last_plane.UsePlaneScalar(false);
    current_layer->UsePlaneScalar(false);
    last_plane.ResetSourceRectToDisplayFrame();
    const HwcRect<float> &current_rect = last_plane.GetSourceCrop();
    for (size_t i = 0; i < size; i++) {
      surfaces.at(i)->ResetSourceCrop(current_rect);
      surfaces.at(i)->GetLayer()->UsePlaneScalar(false);
    }
  }

  // TODO: Handle case where all layers to be compoisted have same scaling
  // ratio.
  // We cannot use plane scaling for Layers with different scaling ratio.
  if (total_layers > 1) {
    return;
  }

  uint32_t display_frame_width = current_layer->GetDisplayFrameWidth();
  uint32_t display_frame_height = current_layer->GetDisplayFrameHeight();
  uint32_t source_crop_width = current_layer->GetSourceCropWidth();
  uint32_t source_crop_height = current_layer->GetSourceCropHeight();
  // Source and Display frame width, height are same and scaling is not needed.
  if ((display_frame_width == source_crop_width) &&
      (display_frame_height == source_crop_height)) {
    return;
  }

  // Case where we are not rotating the layer and format is supported by the
  // plane.
  // If we are here this means the layer cannot be scaled using display, just
  // return.
  if (!ignore_format &&
      (current_layer->GetPlaneTransform() == HWCTransform::kIdentity) &&
      last_plane.plane()->IsSupportedFormat(
          current_layer->GetBuffer()->GetFormat())) {
    return;
  }

  // Display frame width, height is lesser than Source. Let's downscale
  // it with our compositor backend.
  if ((display_frame_width < source_crop_width) &&
      (display_frame_height < source_crop_height)) {
    return;
  }

  // Display frame height is less. If the cost of upscaling width is less
  // than downscaling height, than return.
  if ((display_frame_width > source_crop_width) &&
      (display_frame_height < source_crop_height)) {
    uint32_t width_cost =
        (display_frame_width - source_crop_width) * display_frame_height;
    uint32_t height_cost =
        (source_crop_height - display_frame_height) * display_frame_width;
    if (height_cost > width_cost) {
      return;
    }
  }

  // Display frame width is less. If the cost of upscaling height is less
  // than downscaling width, than return.
  if ((display_frame_width < source_crop_width) &&
      (display_frame_height > source_crop_height)) {
    uint32_t width_cost =
        (source_crop_width - display_frame_width) * display_frame_height;
    uint32_t height_cost =
        (display_frame_height - source_crop_height) * display_frame_width;
    if (width_cost > height_cost) {
      return;
    }
  }

  // TODO: Scalars are limited in HW. Determine scaling ratio
  // which would really benefit vs doing it in GPU side.

  // Display frame and Source rect are different, let's check if
  // we can take advantage of scalars attached to this plane.
  const HwcRect<float> &crop = current_layer->GetSourceCrop();
  last_plane.SetSourceCrop(crop);
  for (size_t i = 0; i < size; i++) {
    surfaces.at(i)->ResetSourceCrop(crop);
    surfaces.at(i)->GetLayer()->UsePlaneScalar(true);
  }

  OverlayPlane &last_overlay_plane = commit_planes.back();
  last_overlay_plane.layer = last_plane.GetOverlayLayer();

  bool fall_back =
      FallbacktoGPU(last_plane.plane(),
                    last_plane.GetOffScreenTarget()->GetLayer(), commit_planes);
  if (fall_back) {
    last_plane.ResetSourceRectToDisplayFrame();
    const HwcRect<float> &current_rect = last_plane.GetSourceCrop();
    for (size_t i = 0; i < size; i++) {
      surfaces.at(i)->ResetSourceCrop(current_rect);
      surfaces.at(i)->GetLayer()->UsePlaneScalar(false);
    }
  } else {
    last_plane.UsePlaneScalar(true);
    current_layer->UsePlaneScalar(true);
  }
}

void DisplayPlaneManager::ResetPlaneTarget(DisplayPlaneState &plane,
                                           OverlayPlane &overlay_plane) {
  SetOffScreenPlaneTarget(plane);
  overlay_plane.layer = plane.GetOverlayLayer();
}

void DisplayPlaneManager::SetOffScreenPlaneTarget(DisplayPlaneState &plane) {
  EnsureOffScreenTarget(plane);

  // Case where we have just one layer which needs to be composited using
  // GPU.
  plane.ForceGPURendering();
}

void DisplayPlaneManager::SetOffScreenCursorPlaneTarget(
    DisplayPlaneState &plane, uint32_t width, uint32_t height) {
  NativeSurface *surface = NULL;
  uint32_t preferred_format = plane.plane()->GetPreferredFormat();
  for (auto &fb : cursor_surfaces_) {
    if (!fb->InUse()) {
      uint32_t surface_format = fb->GetLayer()->GetBuffer()->GetFormat();
      if (preferred_format == surface_format) {
        surface = fb.get();
        break;
      }
    }
  }

  if (!surface) {
    NativeSurface *new_surface = Create3DBuffer(width, height);
    new_surface->Init(resource_manager_, preferred_format, true);
    cursor_surfaces_.emplace_back(std::move(new_surface));
    surface = cursor_surfaces_.back().get();
  }

  surface->SetPlaneTarget(plane, gpu_fd_);
  plane.SetOffScreenTarget(surface);
  plane.ForceGPURendering();
}

void DisplayPlaneManager::ReleaseAllOffScreenTargets() {
  CTRACE();
  std::vector<std::unique_ptr<NativeSurface>>().swap(surfaces_);
  std::vector<std::unique_ptr<NativeSurface>>().swap(cursor_surfaces_);
}

void DisplayPlaneManager::ReleaseFreeOffScreenTargets() {
  std::vector<std::unique_ptr<NativeSurface>> surfaces;
  std::vector<std::unique_ptr<NativeSurface>> cursor_surfaces;
  for (auto &fb : surfaces_) {
    if (fb->InUse()) {
      surfaces.emplace_back(fb.release());
    }
  }

  for (auto &cursor_fb : cursor_surfaces_) {
    if (cursor_fb->InUse()) {
      cursor_surfaces.emplace_back(cursor_fb.release());
    }
  }

  surfaces.swap(surfaces_);
  cursor_surfaces.swap(cursor_surfaces_);
}

void DisplayPlaneManager::EnsureOffScreenTarget(DisplayPlaneState &plane) {
  NativeSurface *surface = NULL;
  bool video_separate = plane.IsVideoPlane();
  uint32_t preferred_format = 0;
  if (video_separate) {
    preferred_format = plane.plane()->GetPreferredVideoFormat();
  } else {
    preferred_format = plane.plane()->GetPreferredFormat();
  }

  for (auto &fb : surfaces_) {
    if (!fb->InUse()) {
      uint32_t surface_format = fb->GetLayer()->GetBuffer()->GetFormat();
      if (preferred_format == surface_format) {
        surface = fb.get();
        break;
      }
    }
  }

  if (!surface) {
    NativeSurface *new_surface = NULL;
    if (video_separate) {
      new_surface = CreateVideoBuffer(width_, height_);
    } else {
      new_surface = Create3DBuffer(width_, height_);
    }

    new_surface->Init(resource_manager_, preferred_format);
    surfaces_.emplace_back(std::move(new_surface));
    surface = surfaces_.back().get();
  }

  surface->SetPlaneTarget(plane, gpu_fd_);
  plane.SetOffScreenTarget(surface);
}

void DisplayPlaneManager::ValidateFinalLayers(
    DisplayPlaneStateList &composition, std::vector<OverlayLayer> &layers) {
  std::vector<OverlayPlane> commit_planes;
  for (DisplayPlaneState &plane : composition) {
    if (plane.NeedsOffScreenComposition() && !plane.GetOffScreenTarget()) {
      EnsureOffScreenTarget(plane);
    }

    commit_planes.emplace_back(
        OverlayPlane(plane.plane(), plane.GetOverlayLayer()));
  }

  // If this combination fails just fall back to 3D for all layers.
  if (!plane_handler_->TestCommit(commit_planes)) {
    // We start off with Primary plane.
    DisplayPlane *current_plane = primary_plane_;
    for (DisplayPlaneState &plane : composition) {
      if (plane.GetOffScreenTarget()) {
        plane.GetOffScreenTarget()->SetInUse(false);
      }
    }

    DisplayPlaneStateList().swap(composition);
    auto layer_begin = layers.begin();
    OverlayLayer *primary_layer = &(*(layer_begin));
    commit_planes.emplace_back(OverlayPlane(current_plane, primary_layer));
    composition.emplace_back(current_plane, primary_layer,
                             primary_layer->GetZorder());
    current_plane->SetInUse(true);
    DisplayPlaneState &last_plane = composition.back();
    last_plane.ForceGPURendering();
    ++layer_begin;

    for (auto i = layer_begin; i != layers.end(); ++i) {
      last_plane.AddLayer(&(*(i)));
    }

    EnsureOffScreenTarget(last_plane);
    ReleaseFreeOffScreenTargets();
  }
}

bool DisplayPlaneManager::FallbacktoGPU(
    DisplayPlane *target_plane, OverlayLayer *layer,
    const std::vector<OverlayPlane> &commit_planes) const {
  if (!target_plane->ValidateLayer(layer))
    return true;

  if (layer->GetBuffer()->GetFb() == 0) {
    if (!layer->GetBuffer()->CreateFrameBuffer(gpu_fd_)) {
      return true;
    }
  }

  // TODO(kalyank): Take relevant factors into consideration to determine if
  // Plane Composition makes sense. i.e. layer size etc
  if (!plane_handler_->TestCommit(commit_planes)) {
    return true;
  }

  return false;
}

bool DisplayPlaneManager::CheckPlaneFormat(uint32_t format) {
  return primary_plane_->IsSupportedFormat(format);
}

}  // namespace hwcomposer
