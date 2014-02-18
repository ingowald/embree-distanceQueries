// ======================================================================== //
// Copyright 2009-2013 Intel Corporation                                    //
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

#include "../common/tutorial/tutorial_device.h"

#define USE_INTERSECTION_FILTER 0
#define USE_OCCLUSION_FILTER 0
#define USE_EYELIGHT_SHADING 0

/* accumulation buffer */
Vec3fa* g_accu = NULL;
size_t g_accu_width = 0;
size_t g_accu_height = 0;
size_t g_accu_count = 0;
Vec3f g_accu_vx = zero;
Vec3f g_accu_vy = zero;
Vec3f g_accu_vz = zero;
Vec3f g_accu_p  = zero;
extern bool g_changed;

/* light settings */
extern Vec3fa g_dirlight_direction;
extern Vec3fa g_dirlight_intensity;
extern Vec3fa g_ambient_intensity;

/* hair material */
const Vec3fa hair_K  = Vec3fa(1.0f,0.57f,0.32);
const Vec3fa hair_dK = Vec3fa(0.02f,0.05f,0.02);
//const Vec3fa hair_K  = Vec3fa(1.0f,0.87f,0.62);
const Vec3fa hair_Kr = 0.7f*hair_K;    //!< reflectivity of hair
const Vec3fa hair_Kt = 0.3f*hair_K;    //!< transparency of hair
const float  hair_Ke = 0.01f;
const Vec3fa hair_Kts= hair_Kt; //Vec3fa(pow(hair_Kt.x,hair_Ke),pow(hair_Kt.x,hair_Ke),pow(hair_Kt.x,hair_Ke));    //!< transparency of hair for shadow rays

struct ISPCTriangle 
{
  int v0;                /*< first triangle vertex */
  int v1;                /*< second triangle vertex */
  int v2;                /*< third triangle vertex */
  int materialID;        /*< material of triangle */
};

struct ISPCMaterial
{
  int illum;             /*< illumination model */
  
  float d;               /*< dissolve factor, 1=opaque, 0=transparent */
  float Ns;              /*< specular exponent */
  float Ni;              /*< optical density for the surface (index of refraction) */
  
  Vec3f Ka;              /*< ambient reflectivity */
  Vec3f Kd;              /*< diffuse reflectivity */
  Vec3f Ks;              /*< specular reflectivity */
  Vec3f Tf;              /*< transmission filter */
};

struct ISPCHair
{
  int vertex;
  int id;
};

struct ISPCHairSet
{
  Vec3fa* v;       //!< hair control points (x,y,z,r)
  ISPCHair* hairs; //!< for each hair, index to first control point
  int numVertices;
  int numHairs;
};

struct ISPCMesh
{
  Vec3fa* positions;    //!< vertex position array
  Vec3fa* normals;       //!< vertex normal array
  Vec2f* texcoords;     //!< vertex texcoord array
  ISPCTriangle* triangles;  //!< list of triangles
  int numVertices;
  int numTriangles;
};

struct ISPCScene
{
  ISPCMesh** meshes;         //!< list of meshes
  ISPCMaterial* materials;  //!< material list
  int numMeshes;
  int numMaterials;
  ISPCHairSet** hairs;
  int numHairSets;
};

/* scene data */
extern "C" ISPCScene* g_ispc_scene;
RTCScene g_scene = NULL;

/* render function to use */
renderPixelFunc renderPixel;

Vec3fa renderPixelTestEyeLight(int x, int y, const Vec3fa& vx, const Vec3fa& vy, const Vec3fa& vz, const Vec3fa& p);

__forceinline Vec3fa evalBezier(const int geomID, const int primID, const float t)
{
  const float t0 = 1.0f - t, t1 = t;
  const ISPCHairSet* hair = g_ispc_scene->hairs[geomID]; // FIXME: works only because hairs are added first to scene
  const Vec3fa* vertices = hair->v;
  const ISPCHair* hairs = hair->hairs;
  
  const int i = hairs[primID].vertex;
  const Vec3fa p00 = *(Vec3fa*)&vertices[i+0];
  const Vec3fa p01 = *(Vec3fa*)&vertices[i+1];
  const Vec3fa p02 = *(Vec3fa*)&vertices[i+2];
  const Vec3fa p03 = *(Vec3fa*)&vertices[i+3];

  const Vec3fa p10 = p00 * t0 + p01 * t1;
  const Vec3fa p11 = p01 * t0 + p02 * t1;
  const Vec3fa p12 = p02 * t0 + p03 * t1;
  const Vec3fa p20 = p10 * t0 + p11 * t1;
  const Vec3fa p21 = p11 * t0 + p12 * t1;
  const Vec3fa p30 = p20 * t0 + p21 * t1;
  
  return p30;
  //tangent = p21-p20;
}

struct HitList;

/* extended ray structure that includes total transparency along the ray */
struct RTCRay2
{
  Vec3fa org;     //!< Ray origin
  Vec3fa dir;     //!< Ray direction
  float tnear;   //!< Start of ray segment
  float tfar;    //!< End of ray segment
  float time;    //!< Time of this ray for motion blur.
  int mask;      //!< used to mask out objects during traversal
  Vec3fa Ng;      //!< Geometric normal.
  float u;       //!< Barycentric u coordinate of hit
  float v;       //!< Barycentric v coordinate of hit
  int geomID;    //!< geometry ID
  int primID;    //!< primitive ID
  int instID;    //!< instance ID

  // ray extensions
  RTCFilterFunc filter;
  Vec3fa transparency; //!< accumulated transparency value
  HitList* list;
};

struct HitList
{
  RTCRay2 data[128];
  RTCRay2* rays[128];
  size_t num;
};

bool addHit(HitList* list, RTCRay2& ray)
{
  if (list->num >= 128) 
    return false;

  int i = list->num++;
  list->data[i] = ray;
  RTCRay2* r = &list->data[i];
  while (i>0 && list->rays[i-1]->tfar > r->tfar) {
    list->rays[i] = list->rays[i-1];
    i--;
  }
  list->rays[i] = r;
  return true;
}

/*! random number generator for floating point numbers in range [0,1] */
inline float frand(int& seed) {
  seed = 7 * seed + 5;
  /*seed = 13 * seed + 17;
  seed = 3 * seed + 2;
  seed = 127 * seed + 13;*/
  return (seed & 0xFFFF)/(float)0xFFFF;
  //return drand48();
}

/*! Uniform hemisphere sampling. Up direction is the z direction. */
Vec3fa sampleSphere(const float u, const float v) 
{
  const float phi = 2.0f*(float)pi * u;
  const float cosTheta = 1.0f - 2.0f * v, sinTheta = 2.0f * sqrt(v * (1.0f - v));
  return Vec3fa(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta, float(one_over_four_pi));
}

Vec3f noise(int i, Vec3f p, float t) {
  //return div(p,length(p));
  //return p + Vec3f(sin(2.0f*t),4.0f*t,cos(2.0f*t));
  if (i % 2)
    return p + Vec3f(2.0f*t,2.0f*t,2.0f*t);
  else
    return p + Vec3f(-2.0f*t,2.0f*t,-2.0f*t);
  //return p + Vec3f(4.0f*t,4.0f*t,0.0f);
}

bool enableFilterDispatch = false;

/* filter dispatch function */
void filterDispatch(void* ptr, RTCRay2& ray) {
  if (!enableFilterDispatch) return;
  if (ray.filter) ray.filter(ptr,(RTCRay&)ray);
}

/* intersection filter function */
void intersectionFilter(void* ptr, RTCRay2& ray)
{
  bool added = addHit(ray.list,ray);
  /*if (T != 0.0f && added)*/ ray.geomID = RTC_INVALID_GEOMETRY_ID; // FIXME: enable this
}

/* occlusion filter function */
void occlusionFilter(void* ptr, RTCRay2& ray)
{
  /* make all surfaces opaque */
  if (ray.geomID >= g_ispc_scene->numHairSets) {
    ray.transparency = Vec3fa(zero);
    return;
  }

  /* calculate how much the curve occludes the ray */
  //float sizeRay = max(ray.org.w + ray.tfar*ray.dir.w, 0.00001f);
  //float sizeCurve = evalBezier(ray.geomID,ray.primID,ray.u).w;
  //1.0f-clamp((1.0f-T_hair)*sizeCurve/sizeRay,0.0f,1.0f);

  Vec3fa T = hair_Kts;
  T *= ray.transparency;
  ray.transparency = T;
  if (T != Vec3fa(0.0f)) ray.geomID = RTC_INVALID_GEOMETRY_ID;
}

#if USE_OCCLUSION_FILTER

Vec3fa occluded(RTCScene scene, RTCRay2& ray)
{
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = -1;
  ray.time = 0;
  ray.filter = (RTCFilterFunc) occlusionFilter;
  ray.transparency = Vec3fa(1.0f);
  rtcOccluded(scene,(RTCRay&)ray);
  return ray.transparency;
}

#else

Vec3fa occluded(RTCScene scene, RTCRay2& ray)
{
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = -1;
  ray.time = 0;
  ray.filter = NULL;

  Vec3fa T = 1.0f;
  while (true) 
  {
    //PRINT(ray.tnear);
    rtcIntersect(scene,(RTCRay&)ray);
    //PRINT2(ray.geomID,ray.primID);
    if (ray.geomID == RTC_INVALID_GEOMETRY_ID) break;
    if (ray.geomID >= g_ispc_scene->numHairSets) return 0.0f; // make all surfaces opaque
    
    /* calculate how much the curve occludes the ray */
    //float sizeRay = max(ray.org.w + ray.tfar*ray.dir.w, 0.00001f);
    //float sizeCurve = evalBezier(ray.geomID,ray.primID,ray.u).w;
    T *= hair_Kt; //1.0f-clamp((1.0f-T_hair)*sizeCurve/sizeRay,0.0f,1.0f);

    /* continue ray ray */
    ray.geomID = RTC_INVALID_GEOMETRY_ID;
    ray.tnear = 1.001f*ray.tfar;
    ray.tfar = inf;
  }
  return T;
}

#endif


/* adds hair to the scene */
void addHair (ISPCScene* scene)
{
  int seed = 879;
  const int numCurves = 10;
  //const int numCurves = 1000;
  const int numCurveSegments = 1;
  const int numCurvePoints = 3*numCurveSegments+1;
  const float R = 0.01f;

  ISPCHairSet* hair = new ISPCHairSet;
  hair->numVertices = numCurves*numCurvePoints;
  hair->numHairs = numCurves*numCurveSegments;
  hair->v = new Vec3fa[hair->numVertices];
  hair->hairs = new ISPCHair[hair->numHairs];
  Vertex* vertices = (Vertex*) hair->v;
  ISPCHair* hairs  = hair->hairs;

  for (size_t i=0; i<numCurves; i++)
  {
    float ru = frand(seed);
    float rv = frand(seed);
    Vec3f p = Vec3f(-4.0f+ru*8.0f,-2.0f,-4.0f+rv*8.0f);
    for (size_t j=0; j<=numCurveSegments; j++) 
    {
      bool last = j == numCurveSegments;
      float f0 = float(2*j+0)/float(2*numCurveSegments);
      float f1 = float(2*j+1)/float(2*numCurveSegments);
      Vec3f p0 = noise(i,p,f0);
      Vec3f p1 = noise(i,p,f1);
      
      if (j>0) {
        vertices[i*numCurvePoints+3*j-1].x = 2.0f*p0.x-p1.x;
        vertices[i*numCurvePoints+3*j-1].y = 2.0f*p0.y-p1.y;
        vertices[i*numCurvePoints+3*j-1].z = 2.0f*p0.z-p1.z;
        vertices[i*numCurvePoints+3*j-1].r = last ? 0.0f : R;
      }
      
      vertices[i*numCurvePoints+3*j+0].x = p0.x;
      vertices[i*numCurvePoints+3*j+0].y = p0.y;
      vertices[i*numCurvePoints+3*j+0].z = p0.z;
      vertices[i*numCurvePoints+3*j+0].r = last ? 0.0f : R;

      if (j<numCurveSegments) {
        vertices[i*numCurvePoints+3*j+1].x = p1.x;
        vertices[i*numCurvePoints+3*j+1].y = p1.y;
        vertices[i*numCurvePoints+3*j+1].z = p1.z;
        vertices[i*numCurvePoints+3*j+1].r = R;
      }
    }

    for (size_t j=0; j<numCurveSegments; j++) {
      hairs[i*numCurveSegments+j].vertex = i*numCurvePoints+3*j;
      hairs[i*numCurveSegments+j].id = i;
    }
  }

  scene->hairs[scene->numHairSets++] = hair;
}

/* adds a ground plane to the scene */
void addGroundPlane (ISPCScene* scene)
{
  ISPCMesh* mesh = new ISPCMesh;
  mesh->positions = new Vec3fa[4];
  mesh->normals = NULL;
  mesh->texcoords = NULL;
  mesh->triangles = new ISPCTriangle[2];
  mesh->numVertices = 4;
  mesh->numTriangles = 2;

  /* set vertices */
  Vertex* vertices = (Vertex*) mesh->positions;
  vertices[0].x = -10; vertices[0].y = -2; vertices[0].z = -10; 
  vertices[1].x = -10; vertices[1].y = -2; vertices[1].z = +10; 
  vertices[2].x = +10; vertices[2].y = -2; vertices[2].z = -10; 
  vertices[3].x = +10; vertices[3].y = -2; vertices[3].z = +10;
  
  /* set triangles */
  ISPCTriangle* triangles = (ISPCTriangle*) mesh->triangles;
  triangles[0].v0 = 0; triangles[0].v1 = 2; triangles[0].v2 = 1; triangles[0].materialID = 0;
  triangles[1].v0 = 1; triangles[1].v1 = 2; triangles[1].v2 = 3; triangles[1].materialID = 0;

  scene->meshes[scene->numMeshes++] = mesh;
}

RTCScene convertScene(ISPCScene* scene_in)
{
  /* create scene */
  RTCScene scene_out = rtcNewScene(RTC_SCENE_STATIC | RTC_SCENE_INCOHERENT,RTC_INTERSECT1);

  /* add all hair sets to the scene */
  //scene_in->numHairSets = 0;
  for (int i=0; i<scene_in->numHairSets; i++)
  {
    /* get ith hair set */
    ISPCHairSet* hair = scene_in->hairs[i];
    PRINT(hair->numHairs);
    
    /* create a hair set */
    unsigned int geomID = rtcNewBezierCurves (scene_out, RTC_GEOMETRY_STATIC, hair->numHairs, hair->numVertices);
    rtcSetBuffer(scene_out,geomID,RTC_VERTEX_BUFFER,hair->v,0,sizeof(Vertex));
    rtcSetBuffer(scene_out,geomID,RTC_INDEX_BUFFER,hair->hairs,0,sizeof(ISPCHair));
#if USE_INTERSECTION_FILTER
  rtcSetIntersectionFilterFunction(scene_out,geomID,(RTCFilterFunc)filterDispatch);
#endif
#if USE_OCCLUSION_FILTER
  rtcSetOcclusionFilterFunction(scene_out,geomID,(RTCFilterFunc)filterDispatch);
#endif
  }

#if 1
  /* add all meshes to the scene */
  for (int i=0; i<scene_in->numMeshes; i++)
  {
    /* get ith mesh */
    ISPCMesh* mesh = scene_in->meshes[i];
    PRINT(mesh->numTriangles);

    /* create a triangle mesh */
    unsigned int geomID = rtcNewTriangleMesh (scene_out, RTC_GEOMETRY_STATIC, mesh->numTriangles, mesh->numVertices);
    
    /* set vertices */
    Vertex* vertices = (Vertex*) rtcMapBuffer(scene_out,geomID,RTC_VERTEX_BUFFER); 
    for (int j=0; j<mesh->numVertices; j++) {
      vertices[j].x = mesh->positions[j].x;
      vertices[j].y = mesh->positions[j].y;
      vertices[j].z = mesh->positions[j].z;
    }

    /* set triangles */
    Triangle* triangles = (Triangle*) rtcMapBuffer(scene_out,geomID,RTC_INDEX_BUFFER);
    for (int j=0; j<mesh->numTriangles; j++) {
      triangles[j].v0 = mesh->triangles[j].v0;
      triangles[j].v1 = mesh->triangles[j].v1;
      triangles[j].v2 = mesh->triangles[j].v2;
    }
    rtcUnmapBuffer(scene_out,geomID,RTC_VERTEX_BUFFER); 
    rtcUnmapBuffer(scene_out,geomID,RTC_INDEX_BUFFER);

#if USE_INTERSECTION_FILTER
  rtcSetIntersectionFilterFunction(scene_out,geomID,(RTCFilterFunc)filterDispatch);
#endif
#if USE_OCCLUSION_FILTER
  rtcSetOcclusionFilterFunction(scene_out,geomID,(RTCFilterFunc)filterDispatch);
#endif
  }
#endif

  /* commit changes to scene */
  rtcCommit (scene_out);
  return scene_out;
}

/* called by the C++ code for initialization */
extern "C" void device_init (int8* cfg)
{
  /* initialize ray tracing core */
  rtcInit(cfg);

  /* set start render mode */
  renderPixel = renderPixelStandard;
}

/*! Anisotropic power cosine microfacet distribution. */
class AnisotropicBlinn {
public:

  __forceinline AnisotropicBlinn() {}

  /*! Anisotropic power cosine distribution constructor. */
  __forceinline AnisotropicBlinn(const Vec3fa& Kr, const Vec3fa& Kt, const Vec3fa& dx, float nx, const Vec3fa& dy, float ny, const Vec3fa& dz) 
    : Kr(Kr), Kt(Kt), dx(dx), nx(nx), dy(dy), ny(ny), dz(dz),
      norm1(sqrtf((nx+1)*(ny+1)) * float(one_over_two_pi)),
      norm2(sqrtf((nx+2)*(ny+2)) * float(one_over_two_pi)),
      side(reduce_max(Kr)/(reduce_max(Kr)+reduce_max(Kt))) {}

  /*! Evaluates the power cosine distribution. \param wh is the half
   *  vector */
  __forceinline float eval(const Vec3fa& wh) const 
  {
    const float cosPhiH   = dot(wh, dx);
    const float sinPhiH   = dot(wh, dy);
    const float cosThetaH = dot(wh, dz);
    const float R = sqr(cosPhiH)+sqr(sinPhiH);
    if (R == 0.0f) return norm2;
    const float n = (nx*sqr(cosPhiH)+ny*sqr(sinPhiH))*rcp(R);
    return norm2 * pow(abs(cosThetaH), n);
  }

  /*! Samples the distribution. \param s is the sample location
   *  provided by the caller. */
  __forceinline Vec3fa sample(const float sx, const float sy) const
  {
    const float phi = float(two_pi)*sx;
    const float sinPhi0 = sqrtf(nx+1)*sinf(phi);
    const float cosPhi0 = sqrtf(ny+1)*cosf(phi);
    const float norm = rsqrt(sqr(sinPhi0)+sqr(cosPhi0));
    const float sinPhi = sinPhi0*norm;
    const float cosPhi = cosPhi0*norm;
    const float n = nx*sqr(cosPhi)+ny*sqr(sinPhi);
    const float cosTheta = powf(sy,rcp(n+1));
    const float sinTheta = cos2sin(cosTheta);
    const float pdf = max(norm1*powf(cosTheta,n),0.1f); // FIXME: clamping PDF
    const Vec3fa h(cosPhi * sinTheta, sinPhi * sinTheta, cosTheta);
    const Vec3fa wh = h.x*dx + h.y*dy + h.z*dz;
    return Vec3fa(wh,pdf);
  }

  __forceinline Vec3fa reflect(const Vec3fa& I, const Vec3fa& N) const {
    return I-2.0f*dot(I,N)*N;
  }

  __forceinline Vec3fa eval(const Vec3fa& wo, const Vec3fa& wi) const
  {
    const float cosThetaI = dot(wi,dz);
    
    /* reflection */
    if (cosThetaI > 0.0f) {
      const Vec3fa wh = normalize(wi + wo);
      return Kr * eval(wh) * abs(cosThetaI);
    } 
    
    /* transmission */
    else {
      const Vec3fa wh = normalize(reflect(wi,dz) + wo);
      return Kt * eval(wh) * abs(cosThetaI);
    }
  }

  __forceinline Vec3fa sample(const Vec3fa& wo, Vec3fa& wi, const float sx, const float sy, const float sz) const
  {
    const Vec3fa wh = sample(sx,sy);

    /* reflection */
    if (sz < side) {
      wi = Vec3fa(reflect(wo,wh),wh.w*side);
      if (dot(wi,dz) < 0.0f) return Vec3fa(zero,0.0f);
      const float cosThetaI = dot(wi,dz);
      return Kr * eval(wh) * abs(cosThetaI);
    }

    /* transmission */
    else {
      wi = Vec3fa(reflect(reflect(wo,wh),dz),wh.w*(1-side));
      if (dot(wi,dz) > 0.0f) return Vec3fa(zero,0.0f);
      const float cosThetaI = dot(wi,dz);
      return Kt * eval(wh) * abs(cosThetaI);
    }
  }

public:
  Vec3fa dx;       //!< x-direction of the distribution.
  float nx;        //!< Glossiness in x direction with range [0,infinity[ where 0 is a diffuse surface.
  Vec3fa dy;       //!< y-direction of the distribution.
  float ny;        //!< Exponent that determines the glossiness in y direction.
  Vec3fa dz;       //!< z-direction of the distribution.
  float norm1;     //!< Normalization constant for calculating the pdf for sampling.
  float norm2;     //!< Normalization constant for calculating the distribution.
  Vec3f Kr,Kt;     // FIXME: using Vec3fa triggers some compiler bug!?
  float side;
};

/* task that renders a single screen tile */
Vec3fa renderPixelStandard(float x, float y, const Vec3fa& vx, const Vec3fa& vy, const Vec3fa& vz, const Vec3fa& p)
{
  int seed = random(); //255*x+13*y+45*g_accu_count;

  /* initialize ray */
  RTCRay2 ray;
  ray.org = p;
  ray.org.w = 0.0f;
  ray.dir = normalize(x*vx + y*vy + vz);
  Vec3fa dir1 = normalize((x+1)*vx + (y+1)*vy + vz);
  ray.dir.w = 0.0f; //0.5f*0.707f*length(dir1-ray.dir);
  ray.tnear = 0.0f;
  ray.tfar = inf;
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = -1;
  ray.time = 0;
  ray.filter = (RTCFilterFunc) intersectionFilter;

  Vec3fa color = Vec3f(0.0f);
  Vec3fa weight = 1.0f;
  size_t depth = 0;

  while (true)
  {
    /* terminate ray path */
    if (reduce_max(weight) < 0.01 || depth > 10) 
      return color; // + weight*g_ambient_intensity;

    /* intersect ray with scene and gather all hits */
    rtcIntersect(g_scene,(RTCRay&)ray);
    
    /* exit if we hit environment */
    if (ray.geomID == RTC_INVALID_GEOMETRY_ID) 
      return color + weight*g_ambient_intensity;

    //return Vec3fa(ray.u,ray.v,0.0f);
  
    /* calculate transmissivity of hair */
    AnisotropicBlinn brdf;

    if (ray.geomID < g_ispc_scene->numHairSets) 
    {
      /* calculate tangent space */
      const Vec3fa dx = normalize(ray.Ng);
      const Vec3fa dy = normalize(cross(ray.dir,dx));
      const Vec3fa dz = normalize(cross(dy,dx));

      /* generate anisotropic BRDF */
      int seed1 = g_ispc_scene->hairs[ray.geomID]->hairs[ray.primID].id;
      const Vec3fa dK = hair_dK*frand(seed1);
      new (&brdf) AnisotropicBlinn(hair_Kr-dK,hair_Kt-dK,dx,10.0f,dy,2.0f,dz);
      brdf.Kr = hair_Kr-dK;
    }
    else 
    {
      if (dot(ray.dir,ray.Ng) > 0) ray.Ng = neg(ray.Ng);

      /* calculate tangent space */
      const Vec3fa dz = normalize(ray.Ng);
      const Vec3fa dx = normalize(cross(dz,ray.dir));
      const Vec3fa dy = normalize(cross(dz,dx));
      
      /* generate isotropic BRDF */
      new (&brdf) AnisotropicBlinn(one,zero,dx,1.0f,dy,1.0f,dz);
    }
    
    /* sample directional light */
    RTCRay2 shadow;
    shadow.org = ray.org + ray.tfar*ray.dir;
    shadow.org.w = 0.0f; //ray.org.w+ray.tfar*ray.dir.w;
    shadow.dir = neg(g_dirlight_direction);
    shadow.dir.w = 0.0f;
    shadow.tnear = 0.001f;
    shadow.tfar = inf;

    shadow.geomID = RTC_INVALID_GEOMETRY_ID;
    shadow.primID = RTC_INVALID_GEOMETRY_ID;
    shadow.mask = -1;
    shadow.time = 0;
    shadow.filter = NULL;

    Vec3fa T = occluded(g_scene,shadow);
    //rtcOccluded(g_scene,(RTCRay&)shadow);
    //Vec3fa T = Vec3fa(one);
    //if (shadow.geomID != RTC_INVALID_GEOMETRY_ID) T = Vec3fa(zero);

    Vec3fa c = brdf.eval(neg(ray.dir),neg(g_dirlight_direction));
    //Vec3fa c = clamp(dot(neg(g_dirlight_direction),brdf.dz),0.0f,1.0f)*float(one_over_pi);
    color += weight*c*T*g_dirlight_intensity;

#if 1
    /* sample BRDF */
    Vec3fa wi;
    c = brdf.sample(neg(ray.dir),wi,frand(seed),frand(seed),frand(seed));
    if (wi.w <= 0.0f) return color;
    ray.org = ray.org + ray.tfar*ray.dir;
    ray.org.w = 0.0f;
    ray.dir = normalize(x*vx + y*vy + vz);
    ray.dir.w = 0.0f;
    ray.tnear = 0.0f;
    ray.tfar = inf;
    ray.geomID = RTC_INVALID_GEOMETRY_ID;
    ray.primID = RTC_INVALID_GEOMETRY_ID;
    ray.mask = -1;
    ray.time = 0;
    ray.filter = NULL; //(RTCFilterFunc) intersectionFilter;
    weight *= c/wi.w;

#else    

    /* continue with transparency ray */
    ray.geomID = RTC_INVALID_GEOMETRY_ID;
    ray.tnear = 1.001f*ray.tfar;
    ray.tfar = inf;
    weight *= brdf.Kt;
#endif

    depth++;
  }
  return color;
}

Vec3fa renderPixelTestEyeLight(float x, float y, const Vec3fa& vx, const Vec3fa& vy, const Vec3fa& vz, const Vec3fa& p)
{
  /* initialize ray */
  RTCRay2 ray;
  ray.org = p;
  ray.org.w = 0.0f;
  ray.dir = normalize(x*vx + y*vy + vz);
  Vec3fa dir1 = normalize((x+1)*vx + (y+1)*vy + vz);
  ray.dir.w = 0.0f; // 0.5f*0.707f*length(dir1-ray.dir);
  ray.tnear = 0.0f;
  ray.tfar = inf;
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = -1;
  ray.time = 0;

  Vec3fa color = Vec3f(0.0f);
  float weight = 1.0f;

  rtcIntersect(g_scene,(RTCRay&)ray);
  ray.filter = NULL; // (RTCFilterFunc) intersectionFilter;

  if (ray.primID != -1)
    color += 0.3f + abs(dot(ray.dir,ray.Ng));

#if 0
    /* sample directional light */
    RTCRay2 shadow;
    shadow.org = ray.org + ray.tfar*ray.dir;
    shadow.org.w = 0.0f; //ray.org.w+ray.tfar*ray.dir.w;
    shadow.dir = neg(g_dirlight_direction);
    shadow.dir.w = 0.0f;
    shadow.tnear = 0.001f;
    shadow.tfar = inf;

    shadow.geomID = RTC_INVALID_GEOMETRY_ID;
    shadow.primID = RTC_INVALID_GEOMETRY_ID;
    shadow.mask = -1;
    shadow.time = 0;
    shadow.filter = NULL;

    //Vec3fa T = occluded(g_scene,shadow);
    rtcIntersect(g_scene,(RTCRay&)shadow);
    Vec3fa T = Vec3fa(one);
    if (shadow.geomID != RTC_INVALID_GEOMETRY_ID) T = Vec3fa(zero);
    color = T;
#endif

  return color;
}

/* task that renders a single screen tile */
void renderTile(int taskIndex, int* pixels,
                const int width,
                const int height, 
                const float time,
                const Vec3f& vx, 
                const Vec3f& vy, 
                const Vec3f& vz, 
                const Vec3f& p,
                const int numTilesX, 
                const int numTilesY)
{
  const int tileY = taskIndex / numTilesX;
  const int tileX = taskIndex - tileY * numTilesX;
  const int x0 = tileX * TILE_SIZE_X;
  const int x1 = min(x0+TILE_SIZE_X,width);
  const int y0 = tileY * TILE_SIZE_Y;
  const int y1 = min(y0+TILE_SIZE_Y,height);
  int seed = tileY*numTilesX+tileX+g_accu_count;

  for (int y = y0; y<y1; y++) for (int x = x0; x<x1; x++)
  {
    //if (x != 341 || y != 472) continue;

    /* calculate pixel color */
    float fx = x + frand(seed);
    float fy = y + frand(seed);
#if USE_EYELIGHT_SHADING == 1
    Vec3f color = renderPixelTestEyeLight(fx,fy,vx,vy,vz,p);
#else
    Vec3f color = renderPixel(fx,fy,vx,vy,vz,p);
#endif

    Vec3fa& dst = g_accu[y*width+x];
    dst += Vec3fa(color.x,color.y,color.z,1.0f);

    /* write color to framebuffer */
    float f = rcp(max(0.001f,dst.w));
    unsigned int r = (unsigned int) (255.0f * clamp(dst.x*f,0.0f,1.0f));
    unsigned int g = (unsigned int) (255.0f * clamp(dst.y*f,0.0f,1.0f));
    unsigned int b = (unsigned int) (255.0f * clamp(dst.z*f,0.0f,1.0f));
    pixels[y*width+x] = (b << 16) + (g << 8) + r;
  }
}

/* called by the C++ code to render */
extern "C" void device_render (int* pixels,
                               const int width,
                               const int height,
                               const float time,
                               const Vec3f& vx, 
                               const Vec3f& vy, 
                               const Vec3f& vz, 
                               const Vec3f& p)
{
  /* create scene */
  if (g_scene == NULL) {
    if (g_ispc_scene == NULL) {
      ISPCScene* scene = new ISPCScene;
      scene->materials = NULL;
      scene->numMaterials = 0;
      scene->meshes = new ISPCMesh*[1024*1024]; // FIXME: hardcoded maximal number of meshes
      scene->numMeshes = 0;
      scene->hairs = new ISPCHairSet*[1024]; // FIXME: hardcoded maximal number of hair sets
      scene->numHairSets = 0;
      g_ispc_scene = scene;
      addHair(scene);
      addGroundPlane(scene);
    }
    g_scene = convertScene(g_ispc_scene);
  }

  /* create accumulator */
  if (g_accu_width != width || g_accu_height != height) {
    g_accu = new Vec3fa[width*height];
    g_accu_width = width;
    g_accu_height = height;
    memset(g_accu,0,width*height*sizeof(Vec3fa));
  }

  /* reset accumulator */
  bool camera_changed = g_changed; g_changed = false;
  camera_changed |= g_accu_vx != vx; g_accu_vx = vx;
  camera_changed |= g_accu_vy != vy; g_accu_vy = vy;
  camera_changed |= g_accu_vz != vz; g_accu_vz = vz;
  camera_changed |= g_accu_p  != p;  g_accu_p  = p;
  g_accu_count++;
  if (camera_changed) {
    g_accu_count=0;
    memset(g_accu,0,width*height*sizeof(Vec3fa));
  }

  /* render frame */
  const int numTilesX = (width +TILE_SIZE_X-1)/TILE_SIZE_X;
  const int numTilesY = (height+TILE_SIZE_Y-1)/TILE_SIZE_Y;
  enableFilterDispatch = true;
  launch_renderTile(numTilesX*numTilesY,pixels,width,height,time,vx,vy,vz,p,numTilesX,numTilesY); 
  enableFilterDispatch = false;
  rtcDebug();
}

/* called by the C++ code for cleanup */
extern "C" void device_cleanup ()
{
  rtcDeleteScene (g_scene);
  rtcExit();
}


///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////


#if 0
/* task that renders a single screen tile */
Vec3fa renderPixelStandard(float x, float y, const Vec3fa& vx, const Vec3fa& vy, const Vec3fa& vz, const Vec3fa& p)
{
  //PRINT2(x,y);
  //if (x != 400 || y != 183) return zero;

  /* initialize ray */
  RTCRay2 ray;
  ray.org = p;
  ray.org.w = 0.0f;
  ray.dir = normalize(x*vx + y*vy + vz);
  Vec3fa dir1 = normalize((x+1)*vx + (y+1)*vy + vz);
  ray.dir.w = 0.5f*0.707f*length(dir1-ray.dir);
  ray.tnear = 0.0f;
  ray.tfar = inf;
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = -1;
  ray.time = 0;
  ray.filter = (RTCFilterFunc) intersectionFilter;

  Vec3fa color = Vec3f(0.0f);
  float weight = 1.0f;

#if USE_INTERSECTION_FILTER

  HitList hits;
  hits.num = 0;
  ray.list = &hits;

  /* intersect ray with scene and gather all hits */
  rtcIntersect(g_scene,(RTCRay&)ray);

  /* iterate through all hits */
  for (size_t i=0; i<hits.num; i++) {
    RTCRay2* ray2 = hits.rays[i];
#else

  while (true)
  {
    /* intersect ray with scene and gather all hits */
    rtcIntersect(g_scene,(RTCRay&)ray);
    RTCRay2* ray2 = &ray;
    
    /* exit if we hit environment */
    if (ray2->geomID == RTC_INVALID_GEOMETRY_ID) 
      return color;

    return Vec3fa(ray2->u,ray2->v,0.0f);
    
#endif
  
    /* calculate transmissivity of hair */
    AnisotropicBlinn brdf;

    float Th = 0.0f;
    if (ray2->geomID < g_ispc_scene->numHairSets) 
    {
      /* calculate how much the curve occludes the ray */
      float sizeRay = max(ray2->org.w + ray2->tfar*ray2->dir.w, 0.00001f);
      float sizeCurve = evalBezier(ray2->geomID,ray2->primID,ray2->u).w;
      Th = 1.0f-clamp((1.0f-T_hair)*sizeCurve/sizeRay,0.0f,1.0f);

      /* calculate tangent space */
      const Vec3fa dx = normalize(ray2->Ng);
      const Vec3fa dy = normalize(cross(ray2->dir,dx));
      const Vec3fa dz = normalize(cross(dy,dx));

      /* generate anisotropic BRDF */
      brdf = AnisotropicBlinn(hair_color,dx,10.0f,dy,1.0f,dz);
    }
    else 
    {
      /* calculate tangent space */
      const Vec3fa dz = -normalize(ray2->Ng);
      const Vec3fa dx = normalize(cross(dz,ray2->dir));
      const Vec3fa dy = normalize(cross(dz,dx));
      
      /* generate isotropic BRDF */
      const Vec3fa color2(1.0f);
      brdf = AnisotropicBlinn(color2,dx,0.0f,dy,0.0f,dz);
    }
    
    /* initialize shadow ray */
    RTCRay2 shadow;
    shadow.org = ray2->org + ray2->tfar*ray2->dir;
    shadow.org.w = ray2->org.w+ray2->tfar*ray2->dir.w;
    shadow.dir = neg(g_dirlight_direction);
    shadow.dir.w = 0.0f;
    shadow.tnear = 0.1f;
    shadow.tfar = inf;
    shadow.geomID = RTC_INVALID_GEOMETRY_ID;
    shadow.primID = RTC_INVALID_GEOMETRY_ID;
    shadow.mask = -1;
    shadow.time = 0;
    shadow.filter = NULL;
    
    /* trace shadow ray */
    float T = occluded(g_scene,shadow);
    //float T = 1.0f;
    
    /* add light contribution */
    //Vec3fa c = Vec3fa(1.0f); 
    Vec3fa c = zero;
    if (brdf.nx == 0.0f || brdf.ny == 0.0f)
      c = Vec3fa(clamp(dot(neg(ray.dir),brdf.dz),0.0f,1.0f));
    else 
      c = brdf.eval(ray.geomID,neg(ray.dir),neg(g_dirlight_direction));

    color += weight*(1.0f-Th)*c*T*g_dirlight_intensity; //clamp(-dot(g_dirlight_direction,normalize(ray.Ng)),0.0f,1.0f))); // FIXME: use +=
    weight *= Th;
    //weight = max(0.0f,weight-Th);
    if (weight < 0.01) return color;
    //return color;

#if !USE_INTERSECTION_FILTER
    /* continue ray */
    ray2->geomID = RTC_INVALID_GEOMETRY_ID;
    ray2->tnear = 1.001f*ray2->tfar; //+2.0f*shadow.org.w;
    ray2->tfar = inf;
#endif
  }
  return color;
}
#endif
