/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "app/Config.h"
#include "app/FilamentApp.h"
#include "app/MeshAssimp.h"

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/driver/PixelBufferDescriptor.h>

#include <utils/EntityManager.h>
#include <utils/Path.h>

#include <image/ColorTransform.h>
#include <imageio/ImageEncoder.h>

#include <math/mat3.h>
#include <math/mat4.h>
#include <math/vec3.h>

#include <getopt/getopt.h>

#include <stb_image.h>
#include <stdlib.h>

#include <math.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace filament::math;
using namespace filament;
using namespace filamat;
using namespace utils;
using namespace image;

const int FRAME_TO_SKIP = 10;

static std::vector<Path> g_filenames;

static std::map<std::string, MaterialInstance*> g_materialInstances;
static std::unique_ptr<MeshAssimp> g_meshSet;
static const Material* g_material;
static Entity g_light;

static bool g_rendered = false;
static int g_currentFrame = 0;

static Config g_config;

static void printUsage(char* name) {
  std::string usage(
      "gltf_renderer generates PNGs of gltf models using the filament "
      "renderer\n"
      "Usage:\n"
      "    gltf_viewer [options] <gltf/glb>\n"
      "Options:\n"
      "   --help, -?\n"
      "       Prints this message\n\n"
      "   --width=<width>, -w <width>\n"
      "       Width of the render\n\n"
      "   --height=<height>, -h <height>\n"
      "       Height of the render\n\n"
      "   --output=<path>, -o <path>\n"
      "       Output path where a PNG of the render will be saved\n\n"
      "   --ibl=<path to cmgen IBL>, -i <path>\n"
      "       Applies an IBL generated by cmgen's deploy option\n\n");
  std::cout << usage;
}

static int handleCommandLineArgments(int argc, char* argv[], Config* config) {
  static constexpr const char* OPTSTR = "?i:w:h:o:";
  static const struct option OPTIONS[] = {
      {"help", no_argument, nullptr, '?'},
      {"ibl", required_argument, nullptr, 'i'},
      {"width", no_argument, nullptr, 'w'},
      {"height", no_argument, nullptr, 'h'},
      {"output", required_argument, nullptr, 'o'},
      {0, 0, 0, 0}  // termination of the option list
  };
  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
    std::string arg(optarg ? optarg : "");
    switch (opt) {
      default:
      case '?':
        printUsage(argv[0]);
        exit(0);
      case 'w':
        config->width = std::stoi(arg);
        break;
      case 'h':
        config->height = std::stoi(arg);
        break;
      case 'o':
        config->outputPath = arg;
        break;
      case 'i':
        config->iblDirectory = arg;
        break;
    }
  }

  return optind;
}

template <typename T>
static LinearImage toLinear(
    size_t w, size_t h, size_t bpr, const uint8_t* src) {
  LinearImage result(w, h, 3);
  math::float3* d = reinterpret_cast<math::float3*>(result.getPixelRef(0, 0));
  for (size_t y = 0; y < h; ++y) {
    T const* p = reinterpret_cast<T const*>(src + y * bpr);
    for (size_t x = 0; x < w; ++x, p += 3) {
      math::float3 sRGB(p[0], p[1], p[2]);
      sRGB /= std::numeric_limits<T>::max();
      *d++ = sRGB;
    }
  }
  return result;
}

static void cleanup(Engine* engine, View* view, Scene* scene) {
  for (auto& item : g_materialInstances) {
    auto materialInstance = item.second;
    engine->destroy(materialInstance);
  }
  g_meshSet.reset(nullptr);
  engine->destroy(g_material);

  EntityManager& em = EntityManager::get();
  engine->destroy(g_light);
  em.destroy(g_light);
}

const float FRAMED_HEIGHT = 10.0f;
const float ROOM_PADDING_SCALE = 1.01f;
const float FOV = 45.0f;

static float roomDepth = 0.0f;

static void setup(Engine* engine, View* view, Scene* scene) {
  g_meshSet = std::make_unique<MeshAssimp>(*engine);
  for (auto& filename : g_filenames) {
    g_meshSet->addFromFile(filename, g_materialInstances, false);
  }

  auto& rcm = engine->getRenderableManager();
  auto& tcm = engine->getTransformManager();

  // Scale and translate the model in a way that matches how ModelScene frames
  // a model.
  // @see src/three-components/ModelScene.js
  float aspect = float(g_config.width) / float(g_config.height);
  float halfWidth = aspect * FRAMED_HEIGHT / 2.0f;

  float3 roomMin(-1.0f * halfWidth, 0.0f, -1.0f * halfWidth);
  float3 roomMax(halfWidth, FRAMED_HEIGHT, halfWidth);
  float3 roomSize(
      roomMax.x - roomMin.x, roomMax.y - roomMin.y, roomMax.z - roomMin.z);
  float3 modelMin = g_meshSet->minBound;
  float3 modelMax = g_meshSet->maxBound;

  float3 modelSize = modelMax - modelMin;

  float3 roomCenter(
      roomMin.x + roomSize.x / 2.0f,
      roomMin.y + roomSize.y / 2.0f,
      roomMin.z + roomSize.z / 2.0f);

  float3 modelCenter(
      modelMin.x + modelSize.x / 2.0f,
      modelMin.y + modelSize.y / 2.0f,
      modelMin.z + modelSize.z / 2.0f);

  float scale = std::min(roomSize.x / modelSize.x, roomSize.y / modelSize.y);
  scale = std::min(scale, roomSize.z / modelSize.z);

  scale /= ROOM_PADDING_SCALE;

  modelCenter *= scale;

  float3 center = (roomCenter - modelCenter);

  auto rooti = tcm.getInstance(g_meshSet->rootEntity);

  tcm.setTransform(
      rooti, mat4f::translate(center) * mat4f::scale(float3(scale)));

  if (modelSize.y >= modelSize.x && modelSize.y >= modelSize.z) {
    roomDepth = std::max(modelSize.x, modelSize.z) * scale * ROOM_PADDING_SCALE;
  } else {
    roomDepth = std::abs(roomSize.z);
  }

  // NOTE(cdata): Leaving these in here for future debugging purposes
  /*
  std::cout << "Half width: " << halfWidth << std::endl;
  std::cout << "Aspect ratio: " << aspect << std::endl;
  std::cout << "Room size: " << roomSize << std::endl;
  std::cout << "Model size (natural): " << modelSize << std::endl;
  std::cout << "Model size (scaled): " << modelSize * scale << std::endl;
  std::cout << "Room center: " << roomCenter << std::endl;
  std::cout << "Model center: " << modelCenter << std::endl;
  std::cout << "Center: " << center << std::endl;
  std::cout << "Scale: " << scale << std::endl;
  std::cout << "Model translation: " << tcm.getTransform(rooti)[3].xyz
            << std::endl;
  */

  for (auto renderable : g_meshSet->getRenderables()) {
    if (rcm.hasComponent(renderable)) {
      auto instance = rcm.getInstance(renderable);
      rcm.setCastShadows(instance, true);
      rcm.setReceiveShadows(instance, true);
      scene->addEntity(renderable);
    }
  }

  g_light = EntityManager::get().create();
  LightManager::Builder(LightManager::Type::SUN)
      .color(Color::toLinear<ACCURATE>(sRGBColor(1.0f, 1.0f, 1.0f)))
      .intensity(110000)
      .direction({0.0, -1, 0.0})
      .sunAngularRadius(1.9f)
      .build(*engine, g_light);

  scene->addEntity(g_light);

  // Adjust the IBL so that it matches the skybox orientation per
  // filament.patch
  FilamentApp& filamentApp = FilamentApp::get();
  IBL* ibl = filamentApp.getIBL();
  ibl->getIndirectLight()->setRotation(mat3f::rotate(M_PI_2, float3{0, 1, 0}));
}

static void preRender(Engine*, View* view, Scene*, Renderer*) {
  // Adjust the camera projection and translation in a way that is similar to
  // what ModelScene does.
  // NOTE(cdata): This might be inefficient to do every prerender, but since we
  // only wait for one frame before exiting it shouldn't matter in practice.
  // Also: while spelunking it appeared that the camera has its projection
  // and position updated multiple times per frame by other implementation
  // outside of our control. This is why we must make these adjustments during
  // preRender.
  // @see src/three-components/ModelScene.js
  float aspect = float(g_config.width) / float(g_config.height);
  float halfWidth = aspect * FRAMED_HEIGHT / 2.0f;
  float near = (FRAMED_HEIGHT / 2.0f) / std::tan((FOV / 2.0f) * M_PI / 180.0f);

  Camera& camera = view->getCamera();

  camera.setProjection(FOV, aspect, near, 100.0f);
  camera.setModelMatrix(mat4f::translate(
      float3(0.0f, FRAMED_HEIGHT / 2.0f, (roomDepth / 2.0f) + near)));
}

static void postRender(Engine*, View* view, Scene*, Renderer* renderer) {
  int frame = g_currentFrame - FRAME_TO_SKIP - 1;
  // Account for the back buffer
  if (frame == 1) {
    std::cout << "Rendering\n";
    const Viewport& vp = view->getViewport();
    uint8_t* pixels = new uint8_t[vp.width * vp.height * 3];

    struct CaptureState {
      View* view = nullptr;
    };

    driver::PixelBufferDescriptor buffer(
        pixels,
        vp.width * vp.height * 3,
        driver::PixelBufferDescriptor::PixelDataFormat::RGB,
        driver::PixelBufferDescriptor::PixelDataType::UBYTE,
        [](void* buffer, size_t size, void* user) {
          if (size > 0) {
            CaptureState* state = static_cast<CaptureState*>(user);
            const Viewport& v = state->view->getViewport();

            LinearImage image(toLinear<uint8_t>(
                v.width, v.height, v.width * 3, static_cast<uint8_t*>(buffer)));

            std::string name = g_config.outputPath;
            Path out(name);

            std::ofstream outputStream(out, std::ios::binary | std::ios::trunc);
            ImageEncoder::encode(
                outputStream, ImageEncoder::Format::PNG, image, "", name);

            delete[] static_cast<uint8_t*>(buffer);
            delete state;

            g_rendered = true;
          }
        },
        new CaptureState{view});

    renderer->readPixels(
        (uint32_t)vp.left,
        (uint32_t)vp.bottom,
        vp.width,
        vp.height,
        std::move(buffer));
  }

  if (g_rendered == true) {
    FilamentApp::get().close();
  }

  g_currentFrame++;
}

// Reconfigures the window dimensions as necessary so that we take consistently
// sized screenshots across all display densities. Note that the render scale is
// not directly related to the display DPI. For example, a MacBook Pro with a
// reported DPI of 129 might use a scaling factor of 2.0.
static void configureWindow(SDL_Window* window) {
  int windowWidth, windowHeight;
  int displayWidth, displayHeight;
  SDL_GetWindowSize(window, &windowWidth, &windowHeight);
  SDL_GL_GetDrawableSize(window, &displayWidth, &displayHeight);

  float renderScale = displayWidth / windowWidth;

  std::cout << "Initial window dimensions: " << windowWidth << " x "
            << windowHeight << std::endl;
  std::cout << "Initial display dimensions: " << displayWidth << " x "
            << displayHeight << std::endl;

  std::cout << "Detected backing scale: " << renderScale << std::endl;

  if (renderScale > 1.0f) {
    int newWidth = windowWidth / renderScale;
    int newHeight = windowHeight / renderScale;
    std::cout << "Resizing window to: " << newWidth << " x " << newHeight
              << std::endl;
    SDL_SetWindowSize(window, newWidth, newHeight);
  }
}

int main(int argc, char* argv[]) {
  int option_index = handleCommandLineArgments(argc, argv, &g_config);
  int num_args = argc - option_index;

  if (num_args < 1) {
    printUsage(argv[0]);
    return 1;
  }

  for (int i = option_index; i < argc; i++) {
    utils::Path filename = argv[i];
    if (!filename.exists()) {
      std::cerr << "file " << argv[option_index] << " not found!" << std::endl;
      return 1;
    }
    g_filenames.push_back(filename);
  }

  FilamentApp& filamentApp = FilamentApp::get();
  filamentApp.run(
      g_config,
      configureWindow,
      setup,
      cleanup,
      nullptr,
      preRender,
      postRender,
      g_config.width,
      g_config.height);

  return 0;
}