/*
 * Copyright (c) 2015, Kai Wolf
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

// C system files
// none

// C++ system files
#include <cstdlib>
#include <algorithm>
#include <limits>

// header files of other libraries
#include <opencv2/imgproc/imgproc.hpp>

// header files of project libraries
#include "mc/marching_cubes.hpp"
#include "voxel_carving.hpp"
#include "cv_utils.hpp"
#include "../filtering/segmentation.hpp"
#include "../common/utils.hpp"

using namespace ret;
using namespace ret::rendering;
using namespace ret::filtering;

VoxelCarving::VoxelCarving(const bb_bounds bbox, const std::size_t voxel_dim)
    : voxel_dim_(voxel_dim),
      voxel_slice_(voxel_dim * voxel_dim),
      voxel_size_(voxel_dim * voxel_dim * voxel_dim),
      vox_array_(ret::make_unique<float[]>(voxel_size_)),
      visual_hull_(),
      params_(calcStartParameter(bbox)) {
    std::fill_n(vox_array_.get(), voxel_size_,
                std::numeric_limits<float>::max());
}

void VoxelCarving::carve(const Camera& cam) {

    const cv::Mat Mask = cam.getMask();
    const cv::Mat DistImage = Segmentation::createDistMap(Mask);
    const cv::Size img_size = Mask.size();

    for (std::size_t i = 0; i < voxel_dim_; ++i) {
        for (std::size_t j = 0; j < voxel_dim_; ++j) {
            for (std::size_t k = 0; k < voxel_dim_; ++k) {

                voxel v = calcVoxelPosInCamViewFrustum(i, j, k);
                auto coord = project<cv::Point2f, voxel>(cam, v);
                auto dist = -1.0f;
                if (inside(coord, img_size)) {
                    dist = DistImage.at<float>(coord);
                    if (Mask.at<uchar>(coord) == 0) { // outside
                        dist *= -1.0f;
                    }
                }

                const auto idx = k + j * voxel_dim_ + i * voxel_slice_;
                if (dist < vox_array_[idx]) vox_array_[idx] = dist;
            }
        }
    }
}

PolyData VoxelCarving::createVisualHull() {

    auto mc = ret::make_unique<mc::MarchingCubes>();
    auto voxel_dim = static_cast<int>(voxel_dim_);
    mc->setParams(params_.start_x, params_.start_z, params_.start_y,
                  params_.voxel_width, params_.voxel_depth,
                  params_.voxel_height, 0.0f, voxel_dim, voxel_dim, voxel_dim);
    mc->execute(vox_array_.get());
    auto triangles = mc->getTriangles();
    visual_hull_.setTriangles(triangles);
    visual_hull_.setNormals(calcSurfaceNormals(triangles));

    return visual_hull_;
}

void VoxelCarving::exportToDisk() const {

    assert(visual_hull_.getTriangles().size() > 0);
    auto mc = ret::make_unique<mc::MarchingCubes>();
    mc->saveASOBJ("export.obj", visual_hull_.getTriangles(),
                  visual_hull_.getNormals());
}

std::vector<vec3f> VoxelCarving::calcSurfaceNormals(
    const std::vector<triangle>& triangles) const {

    std::vector<vec3f> normals;
    // one surface normal for each vertex in a triangle
    normals.reserve(triangles.size() * 3);
    for (const auto tri : triangles) {
        vec3f n = calcSurfaceNormal(tri.comp.v1, tri.comp.v2, tri.comp.v3);
        normals.emplace_back(n);
    }

    return normals;
}

vec3f VoxelCarving::calcSurfaceNormal(const vec3f& v1, const vec3f& v2,
                                      const vec3f& v3) const {

    vec3f n;
    n.x = (v2.y - v1.y) * (v3.z - v1.z) - (v3.y - v1.y) * (v2.z - v1.z);
    n.y = (v2.z - v1.z) * (v3.x - v1.x) - (v2.x - v1.x) * (v3.z - v1.z);
    n.z = (v2.x - v1.x) * (v3.y - v1.y) - (v3.x - v1.x) * (v2.y - v1.y);
    return n;
}

voxel VoxelCarving::calcVoxelPosInCamViewFrustum(const std::size_t i,
                                                 const std::size_t j,
                                                 const std::size_t k) const {
    voxel v;
    v.xpos = params_.start_x + static_cast<float>(k) * params_.voxel_width;
    v.ypos = params_.start_y + static_cast<float>(j) * params_.voxel_height;
    v.zpos = params_.start_z + static_cast<float>(i) * params_.voxel_depth;
    v.value = 1.0f;

    return v;
}

start_params VoxelCarving::calcStartParameter(const bb_bounds& bbox) const {

    auto MARGIN_X = 0.06f;
    auto MARGIN_Y = 0.20f;

    auto bb_width = std::abs(bbox.xmax - bbox.xmin) * (1.0f + 2.0f * MARGIN_X);
    auto bb_height = std::abs(bbox.ymax - bbox.ymin) * (1.0f + 2.0f * MARGIN_Y);
    auto bb_depth = std::abs(bbox.zmax - bbox.zmin);

    auto offset_x = (bb_width - std::abs(bbox.xmax - bbox.xmin)) / 2.0f;
    auto offset_y = (bb_height - std::abs(bbox.ymax - bbox.ymin)) / 2.0f;

    start_params params;
    params.start_x = bbox.xmin - offset_x;
    params.start_y = bbox.ymin - offset_y;
    params.start_z = 0.0f;
    params.voxel_width = bb_width / static_cast<float>(voxel_dim_);
    params.voxel_height = bb_height / static_cast<float>(voxel_dim_);
    params.voxel_depth = bb_depth / static_cast<float>(voxel_dim_);

    return params;
}
