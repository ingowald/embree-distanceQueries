// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "../common/tutorial/tutorial_device.isph"

#if 0
const uniform int numSpheres = 1000;
const uniform int numPhi = 5;
#else
const uniform int numSpheres = 20;
const uniform int numPhi = 120;
//const uniform int numPhi = 256;
#endif
const uniform int numTheta = 2*numPhi;

/* scene data */
RTCDevice g_device = NULL;
RTCScene g_scene = NULL;
uniform Vec3f position[numSpheres];
uniform Vec3f colors[numSpheres+1];
uniform float radius[numSpheres];
uniform int disabledID = -1;

/* adds a sphere to the scene */
uniform unsigned int createSphere (uniform RTCGeometryFlags flags, const uniform Vec3f& pos, const uniform float r)
{
  /* create a triangulated sphere */
  uniform unsigned int mesh = rtcNewTriangleMesh (g_scene, flags, 2*numTheta*(numPhi-1), numTheta*(numPhi+1));

  /* map triangle and vertex buffer */
  uniform Vertex*   uniform vertices  = (uniform Vertex*   uniform) rtcMapBuffer(g_scene,mesh,RTC_VERTEX_BUFFER);
  uniform Triangle* uniform triangles = (uniform Triangle* uniform) rtcMapBuffer(g_scene,mesh,RTC_INDEX_BUFFER);

  /* create sphere geometry */
  uniform int tri = 0;
  const uniform float rcpNumTheta = rcp((uniform float)numTheta);
  const uniform float rcpNumPhi   = rcp((uniform float)numPhi);
  for (uniform int phi=0; phi<=numPhi; phi++)
  {
    for (uniform int theta=0; theta<numTheta; theta++)
    {
      const uniform float phif   = phi*pi*rcpNumPhi;
      const uniform float thetaf = theta*2.0f*pi*rcpNumTheta;
      uniform Vertex& v = vertices[phi*numTheta+theta];
      v.x = pos.x + r*sin(phif)*sin(thetaf);
      v.y = pos.y + r*cos(phif);
      v.z = pos.z + r*sin(phif)*cos(thetaf);
    }
    if (phi == 0) continue;

    for (uniform int theta=1; theta<=numTheta; theta++)
    {
      uniform int p00 = (phi-1)*numTheta+theta-1;
      uniform int p01 = (phi-1)*numTheta+theta%numTheta;
      uniform int p10 = phi*numTheta+theta-1;
      uniform int p11 = phi*numTheta+theta%numTheta;

      if (phi > 1) {
        triangles[tri].v0 = p10;
        triangles[tri].v1 = p00;
        triangles[tri].v2 = p01;
        tri++;
      }

      if (phi < numPhi) {
        triangles[tri].v0 = p11;
        triangles[tri].v1 = p10;
        triangles[tri].v2 = p01;
        tri++;
      }
    }
  }
  rtcUnmapBuffer(g_scene,mesh,RTC_VERTEX_BUFFER);
  rtcUnmapBuffer(g_scene,mesh,RTC_INDEX_BUFFER);

  return mesh;
}

/* adds a ground plane to the scene */
uniform unsigned int addGroundPlane (RTCScene scene_i)
{
  /* create a triangulated plane with 2 triangles and 4 vertices */
  uniform unsigned int mesh = rtcNewTriangleMesh (scene_i, RTC_GEOMETRY_STATIC, 2, 4);

  /* set vertices */
  uniform Vertex* uniform vertices = (uniform Vertex* uniform) rtcMapBuffer(scene_i,mesh,RTC_VERTEX_BUFFER);
  vertices[0].x = -10; vertices[0].y = -2; vertices[0].z = -10;
  vertices[1].x = -10; vertices[1].y = -2; vertices[1].z = +10;
  vertices[2].x = +10; vertices[2].y = -2; vertices[2].z = -10;
  vertices[3].x = +10; vertices[3].y = -2; vertices[3].z = +10;
  rtcUnmapBuffer(scene_i,mesh,RTC_VERTEX_BUFFER);

  /* set triangles */
  uniform Triangle* uniform triangles = (uniform Triangle* uniform) rtcMapBuffer(scene_i,mesh,RTC_INDEX_BUFFER);
  triangles[0].v0 = 0; triangles[0].v1 = 2; triangles[0].v2 = 1;
  triangles[1].v0 = 1; triangles[1].v1 = 2; triangles[1].v2 = 3;
  rtcUnmapBuffer(scene_i,mesh,RTC_INDEX_BUFFER);

  return mesh;
}

/* called by the C++ code for initialization */
export void device_init (uniform int8* uniform cfg)
{
  /* create new Embree device */
  g_device = rtcNewDevice(cfg);
  error_handler(rtcDeviceGetError(g_device));

  /* set error handler */
  rtcDeviceSetErrorFunction(g_device,error_handler);

  /* create scene */
  g_scene = rtcDeviceNewScene(g_device,RTC_SCENE_DYNAMIC | RTC_SCENE_ROBUST, RTC_INTERSECT_UNIFORM | RTC_INTERSECT_VARYING);

  /* create some triangulated spheres */
  for (uniform int i=0; i<numSpheres; i++)
  {
    const uniform float phi = i*2.0f*pi/numSpheres;
    const uniform float r = 2.0f*pi/numSpheres;
    const uniform Vec3f p = 2.0f*make_Vec3f(sin(phi),0.0f,-cos(phi));
    //uniform RTCGeometryFlags flags = i%3 == 0 ? RTC_GEOMETRY_STATIC : i%3 == 1 ? RTC_GEOMETRY_DEFORMABLE : RTC_GEOMETRY_DYNAMIC;
    uniform RTCGeometryFlags flags = i%2 ? RTC_GEOMETRY_DEFORMABLE : RTC_GEOMETRY_DYNAMIC;
    //uniform RTCGeometryFlags flags = RTC_GEOMETRY_DEFORMABLE;
    uniform int id = createSphere(flags,p,r);
    position[id] = p;
    radius[id] = r;
    colors[id].x = (i%16+1)/17.0f;
    colors[id].y = (i%8+1)/9.0f;
    colors[id].z = (i%4+1)/5.0f;
  }

  /* add ground plane to scene */
  uniform int id = addGroundPlane(g_scene);
  colors[id] = make_Vec3f(1.0f,1.0f,1.0f);

  /* commit changes to scene */
  rtcCommit (g_scene);

  /* set start render mode */
  renderTile = renderTileStandard;
  key_pressed_handler = device_key_pressed_default;
}

/* animates the sphere */
task void animateSphere (uniform Vertex* uniform vertices,
                         const uniform float rcpNumTheta,
                         const uniform float rcpNumPhi,
                         const uniform Vec3f& pos,
                         const uniform float r,
                         const uniform float f)
{
  uniform int phi = taskIndex;
  foreach (theta = 0 ... numTheta)
  {
    uniform Vertex* v = &vertices[phi*numTheta+theta];
    const float phif   = phi*pi*rcpNumPhi;
    const float thetaf = theta*2.0f*pi*rcpNumTheta;
    v->x = pos.x + r*sin(f*phif)*sin(thetaf);
    v->y = pos.y + r*cos(phif);
    v->z = pos.z + r*sin(f*phif)*cos(thetaf);
  }
}

/* task that renders a single screen tile */
Vec3f renderPixelStandard(float x, float y, const uniform ISPCCamera& camera)
{
  /* initialize ray */
  RTCRay ray;
  ray.org = make_Vec3f(camera.xfm.p);
  ray.dir = make_Vec3f(normalize(x*camera.xfm.l.vx + y*camera.xfm.l.vy + camera.xfm.l.vz));
  ray.tnear = 0.0f;
  ray.tfar = inf;
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = -1;
  ray.time = 0;

  /* intersect ray with scene */
  rtcIntersect(g_scene,ray);

  /* shade pixels */
  Vec3f color = make_Vec3f(0.0f);
  if (ray.geomID != RTC_INVALID_GEOMETRY_ID)
  {
    Vec3f diffuse = colors[ray.geomID];
    color = color + diffuse*0.1f;
    Vec3f lightDir = normalize(make_Vec3f(-1,-1,-1));

    /* initialize shadow ray */
    RTCRay shadow;
    shadow.org = ray.org + ray.tfar*ray.dir;
    shadow.dir = neg(lightDir);
    shadow.tnear = 0.001f;
    shadow.tfar = inf;
    shadow.geomID = 1;
    shadow.primID = 0;
    shadow.mask = -1;
    shadow.time = 0;

    /* trace shadow ray */
    rtcOccluded(g_scene,shadow);

    /* add light contribution */
    if (shadow.geomID)
      color = color + diffuse*clamp(-dot(lightDir,normalize(ray.Ng)),0.0f,1.0f);
  }
  return color;
}

/* renders a single screen tile */
void renderTileStandard(uniform int taskIndex,
                        uniform int* uniform pixels,
                        const uniform unsigned int width,
                        const uniform unsigned int height,
                        const uniform float time,
                        const uniform ISPCCamera& camera,
                        const uniform int numTilesX,
                        const uniform int numTilesY)
{
  const uniform unsigned int tileY = taskIndex / numTilesX;
  const uniform unsigned int tileX = taskIndex - tileY * numTilesX;
  const uniform unsigned int x0 = tileX * TILE_SIZE_X;
  const uniform unsigned int x1 = min(x0+TILE_SIZE_X,width);
  const uniform unsigned int y0 = tileY * TILE_SIZE_Y;
  const uniform unsigned int y1 = min(y0+TILE_SIZE_Y,height);

  foreach_tiled (y = y0 ... y1, x = x0 ... x1)
  {
    /* calculate pixel color */
    Vec3f color = renderPixelStandard((float)x,(float)y,camera);

    /* write color to framebuffer */
    unsigned int r = (unsigned int) (255.0f * clamp(color.x,0.0f,1.0f));
    unsigned int g = (unsigned int) (255.0f * clamp(color.y,0.0f,1.0f));
    unsigned int b = (unsigned int) (255.0f * clamp(color.z,0.0f,1.0f));
    pixels[y*width+x] = (b << 16) + (g << 8) + r;
  }
}

/* task that renders a single screen tile */
task void renderTileTask(uniform int* uniform pixels,
                         const uniform unsigned int width,
                         const uniform unsigned int height,
                         const uniform float time,
                         const uniform ISPCCamera& camera,
                         const uniform int numTilesX,
                         const uniform int numTilesY)
{
  renderTile(taskIndex,pixels,width,height,time,camera,numTilesX,numTilesY);
}

/* animates a sphere */
void animateSphere (uniform int id, uniform float time)
{
  /* animate vertices */
  uniform Vertex* uniform vertices = (uniform Vertex* uniform) rtcMapBuffer(g_scene,id,RTC_VERTEX_BUFFER);
  const uniform float rcpNumTheta = rcp((uniform float)numTheta);
  const uniform float rcpNumPhi   = rcp((uniform float)numPhi);
  const uniform Vec3f pos = position[id];
  const uniform float r = radius[id];
  const uniform float f = 2.0f*(1.0f+0.5f*sin(time));

  /* loop over all vertices */
#if 1 // enables parallel execution
  launch[numPhi+1] animateSphere(vertices,rcpNumTheta,rcpNumPhi,pos,r,f); sync;
#else
  foreach (phi = 0 ... numPhi+1, theta = 0 ... numTheta)
  {
    uniform Vertex* v = &vertices[phi*numTheta+theta];
    const float phif   = phi*pi*rcpNumPhi;
    const float thetaf = theta*2.0f*pi*rcpNumTheta;
    v->x = pos.x+r*sin(f*phif)*sin(thetaf);
    v->y = pos.y+r*cos(phif);
    v->z = pos.z+r*sin(f*phif)*cos(thetaf);
  }
#endif

  rtcUnmapBuffer(g_scene,id,RTC_VERTEX_BUFFER);

  /* update mesh */
  rtcUpdate (g_scene,id);
}

/* called by the C++ code to render */
export void device_render (uniform int* uniform pixels,
                           const uniform unsigned int width,
                           const uniform unsigned int height,
                           const uniform float time,
                           const uniform ISPCCamera& camera)
{
  /* animate sphere */
  for (uniform int i=0; i<numSpheres; i++)
    animateSphere(i,time+i);

  /* commit changes to scene */
  rtcCommit (g_scene);

  /* render all pixels */
  const uniform int numTilesX = (width +TILE_SIZE_X-1)/TILE_SIZE_X;
  const uniform int numTilesY = (height+TILE_SIZE_Y-1)/TILE_SIZE_Y;
  launch[numTilesX*numTilesY] renderTileTask(pixels,width,height,time,camera,numTilesX,numTilesY); sync;
}

/* called by the C++ code for cleanup */
export void device_cleanup ()
{
  rtcDeleteScene (g_scene); g_scene = NULL;
  rtcDeleteDevice(g_device); g_device = NULL;
}
