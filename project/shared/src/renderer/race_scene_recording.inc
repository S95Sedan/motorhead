void RaceRenderer::record_geometry(
    const std::array<SDL_Vertex, 4U> &vertices,
    const std::array<double, 4U> &view_depths,
    const std::uint8_t vertex_count,
    const std::vector<mh::content::PamRgbaImage> *texture_levels,
    const bool trilinear_filtering, const SceneBlendMode blend_mode,
    const bool coplanar_depth_bias, const bool depth_test,
    const std::array<std::array<float, 3U>, 4U> &environment_normals,
    const SceneCarSurface &car_surface) {
  if (vertex_count < 3U || vertex_count > vertices.size()) {
    throw std::runtime_error("race geometry has an invalid vertex count");
  }
#ifndef NDEBUG
  validate_texture_levels(texture_levels);
#endif

  auto minimum_y = 0;
  auto maximum_y = impl_->height - 1;
  if (impl_->backend == RaceRendererBackend::software ||
      impl_->checksum_enabled) {
    minimum_y = impl_->height;
    maximum_y = -1;
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
      const auto y = vertices[vertex].position.y;
      if (!std::isfinite(y)) {
        minimum_y = 0;
        maximum_y = impl_->height - 1;
        break;
      }
      const auto bounded_y = std::clamp(static_cast<double>(y), -1.0,
                                        static_cast<double>(impl_->height));
      minimum_y = std::min(minimum_y, static_cast<int>(std::floor(bounded_y)));
      maximum_y = std::max(maximum_y, static_cast<int>(std::ceil(bounded_y)));
    }
  }

  impl_->commands.emplace_back();
  auto &command = impl_->commands.back();
  command.vertices = vertices;
  command.view_depths = view_depths;
  if (impl_->fog_enabled) {
    const auto fog_range = std::max(0.0001, impl_->fog_end - impl_->fog_start);
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
      const auto normalized = std::clamp(
          (view_depths[vertex] - impl_->fog_start) / fog_range, 0.0, 1.0);
      const auto clear_fraction = 1.0 - normalized;
      command.fog_cues[vertex] =
          static_cast<float>(1.0 - clear_fraction * clear_fraction);
    }
  }
  command.vertex_count = vertex_count;
  command.environment_normals = environment_normals;
  command.car_surface = car_surface;
  command.texture_levels = texture_levels;
  command.trilinear_filtering = trilinear_filtering;
  command.blend_mode = blend_mode;
  command.coplanar_depth_bias = coplanar_depth_bias;
  command.depth_test = depth_test;
  command.minimum_y = std::max(0, minimum_y);
  command.maximum_y = std::min(impl_->height - 1, maximum_y);
}

void RaceRenderer::record_shadow(VehicleShadowDepthProjection projection) {
  const auto index = impl_->shadows.size();
  impl_->shadows.push_back(std::move(projection));

  impl_->commands.emplace_back();
  auto &marker = impl_->commands.back();
  marker.shadow_projection_index = index;
  marker.minimum_y = impl_->shadows.back().minimum_y;
  marker.maximum_y = impl_->shadows.back().maximum_y;
}

void RaceRenderer::record_headlights(HeadlightDepthProjection projection) {
  impl_->headlights.push_back(std::move(projection));
}
