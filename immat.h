#ifndef __IMMAT_H__
#define __IMMAT_H__
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <memory>
#include <mutex>
#include <random>
#if __AVX__
// the alignment of all the allocated buffers
#define IM_MALLOC_ALIGN 32
#else
// the alignment of all the allocated buffers
#define IM_MALLOC_ALIGN 16
#endif

// we have some optimized kernels that may overread buffer a bit in loop
// it is common to interleave next-loop data load with arithmetic instructions
// allocating more bytes keeps us safe from SEGV_ACCERR failure
#define IM_MALLOC_OVERREAD 64

#define OMP_THREADS 8
// exchange-add operation for atomic operations on reference counters
#if defined __riscv && !defined __riscv_atomic
// riscv target without A extension
static inline int IM_XADD(int* addr, int delta)
{
    int tmp = *addr;
    *addr += delta;
    return tmp;
}
#elif defined __INTEL_COMPILER && !(defined WIN32 || defined _WIN32)
// atomic increment on the linux version of the Intel(tm) compiler
#define IM_XADD(addr, delta) (int)_InterlockedExchangeAdd(const_cast<void*>(reinterpret_cast<volatile void*>(addr)), delta)
#elif defined __GNUC__
#if defined __clang__ && __clang_major__ >= 3 && !defined __ANDROID__ && !defined __EMSCRIPTEN__ && !defined(__CUDACC__)
#ifdef __ATOMIC_ACQ_REL
#define IM_XADD(addr, delta) __c11_atomic_fetch_add((_Atomic(int)*)(addr), delta, __ATOMIC_ACQ_REL)
#else
#define IM_XADD(addr, delta) __atomic_fetch_add((_Atomic(int)*)(addr), delta, 4)
#endif
#else
#if defined __ATOMIC_ACQ_REL && !defined __clang__
// version for gcc >= 4.7
#define IM_XADD(addr, delta) (int)__atomic_fetch_add((unsigned*)(addr), (unsigned)(delta), __ATOMIC_ACQ_REL)
#else
#define IM_XADD(addr, delta) (int)__sync_fetch_and_add((unsigned*)(addr), (unsigned)(delta))
#endif
#endif
#elif defined _MSC_VER && !defined RC_INVOKED
#define IM_XADD(addr, delta) (int)_InterlockedExchangeAdd((long volatile*)addr, delta)
#else
// thread-unsafe branch
static inline int IM_XADD(int* addr, int delta)
{
    int tmp = *addr;
    *addr += delta;
    return tmp;
}
#endif

//////////////////////////////////////////////////
//  memory functions
/////////////////////////////////////////////////
template<typename _Tp>
inline _Tp* Im_AlignPtr(_Tp* ptr, int n = (int)sizeof(_Tp))
{
    return (_Tp*)(((size_t)ptr + n - 1) & -n);
}
inline size_t Im_AlignSize(size_t sz, int n)
{
    return (sz + n - 1) & -n;
}
inline void* Im_FastMalloc(size_t size)
{
#if _MSC_VER
    return _aligned_malloc(size, IM_MALLOC_ALIGN);
#elif (defined(__unix__) || defined(__APPLE__)) && _POSIX_C_SOURCE >= 200112L || (__ANDROID__ && __ANDROID_API__ >= 17)
    void* ptr = 0;
    if (posix_memalign(&ptr, IM_MALLOC_ALIGN, size + IM_MALLOC_OVERREAD))
        ptr = 0;
    return ptr;
#elif __ANDROID__ && __ANDROID_API__ < 17
    return memalign(IM_MALLOC_ALIGN, size + IM_MALLOC_OVERREAD);
#else
    unsigned char* udata = (unsigned char*)malloc(size + sizeof(void*) + IM_MALLOC_ALIGN + IM_MALLOC_OVERREAD);
    if (!udata)
        return 0;
    memset(udata, 0, size + sizeof(void*) + IM_MALLOC_ALIGN);
    unsigned char** adata = Im_AlignPtr((unsigned char**)udata + 1, IM_MALLOC_ALIGN);
    adata[-1] = udata;
    return adata;
#endif
}
inline void Im_FastFree(void* ptr)
{
    if (ptr)
    {
#if _MSC_VER
        _aligned_free(ptr);
#elif (defined(__unix__) || defined(__APPLE__)) && _POSIX_C_SOURCE >= 200112L || (__ANDROID__ && __ANDROID_API__ >= 17)
        free(ptr);
#elif __ANDROID__ && __ANDROID_API__ < 17
        free(ptr);
#else
        unsigned char* udata = ((unsigned char**)ptr)[-1];
        free(udata);
#endif
    }
}

//////////////////////////////////////////////////
//  fp16 functions
/////////////////////////////////////////////////
static inline unsigned short im_float32_to_float16(float value)
{
    // 1 : 8 : 23
    union
    {
        unsigned int u;
        float f;
    } tmp;

    tmp.f = value;

    // 1 : 8 : 23
    unsigned short sign = (tmp.u & 0x80000000) >> 31;
    unsigned short exponent = (tmp.u & 0x7F800000) >> 23;
    unsigned int significand = tmp.u & 0x7FFFFF;

    // 1 : 5 : 10
    unsigned short fp16;
    if (exponent == 0)
    {
        // zero or denormal, always underflow
        fp16 = (sign << 15) | (0x00 << 10) | 0x00;
    }
    else if (exponent == 0xFF)
    {
        // infinity or NaN
        fp16 = (sign << 15) | (0x1F << 10) | (significand ? 0x200 : 0x00);
    }
    else
    {
        // normalized
        short newexp = exponent + (-127 + 15);
        if (newexp >= 31)
        {
            // overflow, return infinity
            fp16 = (sign << 15) | (0x1F << 10) | 0x00;
        }
        else if (newexp <= 0)
        {
            // Some normal fp32 cannot be expressed as normal fp16
            fp16 = (sign << 15) | (0x00 << 10) | 0x00;
        }
        else
        {
            // normal fp16
            fp16 = (sign << 15) | (newexp << 10) | (significand >> 13);
        }
    }

    return fp16;
}

static inline float im_float16_to_float32(unsigned short value)
{
    // 1 : 5 : 10
    unsigned short sign = (value & 0x8000) >> 15;
    unsigned short exponent = (value & 0x7c00) >> 10;
    unsigned short significand = value & 0x03FF;

    // 1 : 8 : 23
    union
    {
        unsigned int u;
        float f;
    } tmp;
    if (exponent == 0)
    {
        if (significand == 0)
        {
            // zero
            tmp.u = (sign << 31);
        }
        else
        {
            // denormal
            exponent = 0;
            // find non-zero bit
            while ((significand & 0x200) == 0)
            {
                significand <<= 1;
                exponent++;
            }
            significand <<= 1;
            significand &= 0x3FF;
            tmp.u = (sign << 31) | ((-exponent + (-15 + 127)) << 23) | (significand << 13);
        }
    }
    else if (exponent == 0x1F)
    {
        // infinity or NaN
        tmp.u = (sign << 31) | (0xFF << 23) | (significand << 13);
    }
    else
    {
        // normalized
        tmp.u = (sign << 31) | ((exponent + (-15 + 127)) << 23) | (significand << 13);
    }

    return tmp.f;
}

static inline unsigned short im_float32_to_bfloat16(float value)
{
    // 16 : 16
    union
    {
        unsigned int u;
        float f;
    } tmp;
    tmp.f = value;
    return tmp.u >> 16;
}

static inline float im_bfloat16_to_float32(unsigned short value)
{
    // 16 : 16
    union
    {
        unsigned int u;
        float f;
    } tmp;
    tmp.u = value << 16;
    return tmp.f;
}


////////////////////////////////////////////////////////////////////
// Type define
enum ImDataType {
    IM_DT_UNDEFINED = -1,
    IM_DT_INT8 = 0,
    IM_DT_INT16,
    IM_DT_INT32,
    IM_DT_INT64,
    IM_DT_FLOAT16,
    IM_DT_FLOAT32,
    IM_DT_FLOAT64,
    IM_DT_NB_DATA_TYPE
};

enum ImDataDevice {
    IM_DD_CPU = 0,
    IM_DD_VULKAN,
    IM_DD_VULKAN_IMAGE,
    IM_DD_CUDA,
};

enum ImColorRange {
    IM_CR_FULL_RANGE = 0,
    IM_CR_NARROW_RANGE
};

enum ImColorSpace {
    IM_CS_SRGB = 0,
    IM_CS_BT601,
    IM_CS_BT709,
    IM_CS_BT2020,
    IM_CS_HSV,
    IM_CS_HLS,
    IM_CS_CMY,
    IM_CS_LAB
};

enum ImColorFormat {
    IM_CF_GRAY = 0,
    IM_CF_BGR,
    IM_CF_ABGR,
    IM_CF_BGRA,
    IM_CF_RGB,
    IM_CF_ARGB,
    IM_CF_RGBA,
    IM_CF_YUV420,
    IM_CF_YUV422,
    IM_CF_YUV444,
    IM_CF_YUVA,
    IM_CF_NV12,
    IM_CF_P010LE,
};

enum ImInterpolateMode {
    IM_INTERPOLATE_NEAREST = 0,
    IM_INTERPOLATE_BILINEAR,
    IM_INTERPOLATE_BICUBIC,
    IM_INTERPOLATE_AREA,
    IM_INTERPOLATE_TRILINEAR,
    IM_INTERPOLATE_TETRAHEDRAL,
    IM_NB_INTERP_MODE
};

#define IM_MAT_FLAGS_NONE               (0 << 0)
// 0-7 bits for video
#define IM_MAT_FLAGS_VIDEO_FRAME        (1 << 0)
#define IM_MAT_FLAGS_VIDEO_INTERLACED   (1 << 1)
#define IM_MAT_FLAGS_VIDEO_FRAME_I      (1 << 2)
#define IM_MAT_FLAGS_VIDEO_FRAME_P      (1 << 3)
#define IM_MAT_FLAGS_VIDEO_FRAME_B      (1 << 4)
#define IM_MAT_FLAGS_VIDEO_HDR_PQ       (1 << 5)
#define IM_MAT_FLAGS_VIDEO_HDR_HLG      (1 << 6)
#define IM_MAT_FLAGS_VIDEO_FRAME_UV     (1 << 7)
// 8-15 bits for audio
#define IM_MAT_FLAGS_AUDIO_FRAME        (1 << 8)
//16-23 bits for image
#define IM_MAT_FLAGS_IMAGE_FRAME        (1 << 16)
//24-31 bits for custom
#define IM_MAT_FLAGS_CUSTOM_NORMAL      (1 << 24)
#define IM_MAT_FLAGS_CUSTOM_PREROLL     (1 << 25)
#define IM_MAT_FLAGS_CUSTOM_EOS         (1 << 26)
#define IM_MAT_FLAGS_CUSTOM_INVALID     (1 << 27)
#define IM_MAT_FLAGS_CUSTOM_UNSUPPORTED (1 << 28)


#define IM_ESIZE(a)    (a == IM_DT_INT8 ? (size_t)1u : (a == IM_DT_INT16 || a == IM_DT_FLOAT16) ? (size_t)2u : (a == IM_DT_INT32 || a == IM_DT_FLOAT32) ? (size_t)4u : (a == IM_DT_INT64 || a == IM_DT_FLOAT64) ? (size_t)8u : (size_t)0u)
#define IM_DEPTH(a)    (a == IM_DT_INT8 ? 8 : (a == IM_DT_INT16 || a == IM_DT_FLOAT16) ? 16 : (a == IM_DT_INT32 || a == IM_DT_FLOAT32) ? 32 : (a == IM_DT_INT64 || a == IM_DT_FLOAT64) ? 64 : 0)
#define IM_ISMONO(a)   (a == IM_CF_GRAY)
#define IM_ISRGB(a)    (a == IM_CF_BGR || a == IM_CF_RGB || a == IM_CF_ABGR || a == IM_CF_ARGB)
#define IM_ISYUV(a)    (a == IM_CF_YUV420 || a == IM_CF_YUV422 || a == IM_CF_YUV444 || a == IM_CF_YUVA || a == IM_CF_NV12 || a == IM_CF_P010LE)
#define IM_ISALPHA(a)  (a == IM_CF_ABGR || a == IM_CF_ARGB || a == IM_CF_YUVA)

template<typename T> 
static inline T CLAMP(T v, T mn, T mx) { return (v < mn) ? mn : (v > mx) ? mx : v; }

typedef struct Rational{
    int num; ///< Numerator
    int den; ///< Denominator
} Rational;

enum Ordination {
    ORD_NCWH = 0,
    ORD_NWHC,
    ORD_NCHW,
    ORD_NHWC,
    ORD_NUM
};

struct ImPoint
{
    float                                   x, y;
    constexpr ImPoint()                      : x(0.0f), y(0.0f) { }
    constexpr ImPoint(float _x, float _y)    : x(_x), y(_y) { }
    float  operator[] (size_t idx) const    { assert(idx <= 1); return (&x)[idx]; }
    float& operator[] (size_t idx)          { assert(idx <= 1); return (&x)[idx]; }
    bool operator==(const ImPoint& d) const          { return x == d.x && y == d.y; }
    bool operator==(const ImPoint& d)                { return x == d.x && y == d.y; }
    bool operator!=(const ImPoint& d) const          { return x != d.x || y != d.y; }
};

struct ImPixel
{
    float r, g, b, a;
    constexpr ImPixel() : r(0.0f), g(0.0f), b(0.0f), a(0.0f) { }
    constexpr ImPixel(float _r, float _g, float _b, float _a)  : r(_r), g(_g), b(_b), a(_a) { }
    bool operator==(const ImPixel& d) const          { return r == d.r && g == d.g && b == d.b && a == d.a; }
    bool operator==(const ImPixel& d)                { return r == d.r && g == d.g && b == d.b && a == d.a; }
    bool operator!=(const ImPixel& d) const          { return r != d.r || g != d.g || b != d.b || a != d.a; }
    bool operator!=(const ImPixel& d)                { return r != d.r || g != d.g || b != d.b || a != d.a; }
};
////////////////////////////////////////////////////////////////////

namespace ImGui
{

static inline int GetColorFormatCategory(ImColorFormat fmt)
{
    if (fmt == IM_CF_GRAY)
        return 0;
    else if (fmt >= IM_CF_BGR && fmt <= IM_CF_RGBA)
        return 1;
    else if (fmt >= IM_CF_YUV420 && fmt <= IM_CF_P010LE)
        return 2;
    return -1;
}

static inline int GetChannelCountByColorFormat(ImColorFormat fmt)
{
    switch (fmt)
    {
        case IM_CF_GRAY:
            return 1;
        case IM_CF_YUV420:
        case IM_CF_YUV422:
        case IM_CF_NV12:
        case IM_CF_P010LE:
            return 2;
        case IM_CF_BGR:
        case IM_CF_RGB:
        case IM_CF_YUV444:
            return 3;
        case IM_CF_ABGR:
        case IM_CF_BGRA:
        case IM_CF_ARGB:
        case IM_CF_RGBA:
        case IM_CF_YUVA:
            return 4;
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
// Allocator Class define
//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////

class Allocator
{
public:
    virtual ~Allocator() {};
    virtual void* fastMalloc(size_t size, ImDataDevice device) = 0;
    virtual void* fastMalloc(int w, int h, int c, size_t elemsize, int elempack, ImDataDevice device) = 0;
    virtual void fastFree(void* ptr, ImDataDevice device) = 0;
    virtual int flush(void* ptr, ImDataDevice device) = 0;
    virtual int invalidate(void* ptr, ImDataDevice device) = 0;
};

//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
// ImMat Class define
//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
class ImMat
{
public:
    // empty
    ImMat();
    // vec
    ImMat(int w, size_t elemsize = 4u, Allocator* allocator = 0);
    // image
    ImMat(int w, int h, size_t elemsize = 4u, Allocator* allocator = 0);
    // dim
    ImMat(int w, int h, int c, size_t elemsize = 4u, Allocator* allocator = 0);
    // packed vec
    ImMat(int w, size_t elemsize, int elempack, Allocator* allocator = 0);
    // packed image
    ImMat(int w, int h, size_t elemsize, int elempack, Allocator* allocator = 0);
    // packed dim
    ImMat(int w, int h, int c, size_t elemsize, int elempack, Allocator* allocator = 0);
    // copy
    ImMat(const ImMat& m);
    // external vec
    ImMat(int w, void* data, size_t elemsize = 4u, Allocator* allocator = 0);
    // external image
    ImMat(int w, int h, void* data, size_t elemsize = 4u, Allocator* allocator = 0);
    // external dim
    ImMat(int w, int h, int c, void* data, size_t elemsize = 4u, Allocator* allocator = 0);
    // external packed vec
    ImMat(int w, void* data, size_t elemsize, int elempack, Allocator* allocator = 0);
    // external packed image
    ImMat(int w, int h, void* data, size_t elemsize, int elempack, Allocator* allocator = 0);
    // external packed dim
    ImMat(int w, int h, int c, void* data, size_t elemsize, int elempack, Allocator* allocator = 0);
    // release
    ~ImMat();
    // assign
    ImMat& operator=(const ImMat& m);
    // allocate vec
    void create(int w, size_t elemsize = 4u, Allocator* allocator = 0);
    // allocate image
    void create(int w, int h, size_t elemsize = 4u, Allocator* allocator = 0);
    // allocate dim
    void create(int w, int h, int c, size_t elemsize = 4u, Allocator* allocator = 0);
    // allocate packed vec
    void create(int w, size_t elemsize, int elempack, Allocator* allocator = 0);
    // allocate packed image
    void create(int w, int h, size_t elemsize, int elempack, Allocator* allocator = 0);
    // allocate packed dim
    void create(int w, int h, int c, size_t elemsize, int elempack, Allocator* allocator = 0);
    // allocate vec with type
    void create_type(int w, ImDataType t = IM_DT_INT8, Allocator* allocator = 0);
    // allocate image with type
    void create_type(int w, int h, ImDataType t = IM_DT_INT8, Allocator* allocator = 0);
    // allocate dim with type
    void create_type(int w, int h, int c, ImDataType t = IM_DT_INT8, Allocator* allocator = 0);
    // extern vec with type
    void create_type(int w, void* data, ImDataType t = IM_DT_INT8, Allocator* allocator = 0);
    // extern image with type
    void create_type(int w, int h, void* data, ImDataType t = IM_DT_INT8, Allocator* allocator = 0);
    // extern dim with type
    void create_type(int w, int h, int c, void* data, ImDataType t = IM_DT_INT8, Allocator* allocator = 0);
    // allocate like
    void create_like(const ImMat& m, Allocator* allocator = 0);
    // set all
    template<typename T> void fill(T v);
    // scalar add
    template<typename T> ImMat operator+ (T v);
    template<typename T> ImMat& operator+= (T v);
    // scalar sub
    template<typename T> ImMat operator- (T v);
    template<typename T> ImMat& operator-= (T v);
    // scalar mul
    template<typename T> ImMat operator* (T v);
    template<typename T> ImMat& operator*= (T v);
    // scalar div
    template<typename T> ImMat operator/ (T v);
    template<typename T> ImMat& operator/= (T v);
    // deep copy
    ImMat clone(Allocator* allocator = 0) const;
    // deep copy from other buffer, inplace
    void clone_from(const ImMat& mat, Allocator* allocator = 0);
    // reshape vec
    ImMat reshape(int w, Allocator* allocator = 0) const;
    // reshape image
    ImMat reshape(int w, int h, Allocator* allocator = 0) const;
    // reshape dim
    ImMat reshape(int w, int h, int c, Allocator* allocator = 0) const;
    // transpose
    ImMat t() const;
    // eye
    template<typename T> ImMat& eye(T scale);
    // invert dim = 2 only
    template<typename T> ImMat inv() const;
    // rand
    template<typename T> ImMat& randn(T mean, T stddev);
    // clip
    template<typename T> ImMat& clip(T v_min, T v_max);
    // mat add
    ImMat operator+(const ImMat& mat);
    ImMat& operator+=(const ImMat& mat);
    // mat sub
    ImMat operator-(const ImMat& mat);
    ImMat& operator-=(const ImMat& mat);
    // mat div
    ImMat operator/(const ImMat& mat);
    ImMat& operator/=(const ImMat& mat);
    // mat mul
    ImMat& mul(const ImMat& mat);
    // mat square
    ImMat& square();
    // mat dot mul dims = 2 only
    ImMat operator*(const ImMat& mat);
    ImMat& operator*=(const ImMat& mat);
    // some draw function only support 3 dims
    // mat default ordination is ncwh
    // if need using nwhc then we need set elempack as elemsize * c
    void clean(ImPixel color);
    void get_pixel(int x, int y, ImPixel& color);
    void get_pixel(ImPoint p, ImPixel& color);
    void draw_dot(int x, int y, ImPixel color);
    void draw_dot(ImPoint p, ImPixel color);
    void alphablend(int x, int y, float alpha, ImPixel color);
    void draw_line(float x1, float y1, float x2, float y2, float t, ImPixel color);
    void draw_line(ImPoint p1, ImPoint p2, float t, ImPixel color);
    void draw_circle(float x, float y, float r, ImPixel color);
    void draw_circle(ImPoint p, float r, ImPixel color);
    void draw_circle(float x, float y, float r, float t, ImPixel color);
    void draw_circle(ImPoint p, float r, float t, ImPixel color);
    
    // release
    void release();

    bool empty() const;
    size_t total() const;

    // bits per element
    int elembits() const;

    // shape only
    ImMat shape() const;

    // data reference
    ImMat channel(int c);
    const ImMat channel(int c) const;

    template<typename T>
    inline T* row(int y) { return (T*)((unsigned char*)data + (size_t)w * y * elemsize); }
    template<typename T>
    inline const T* row(int y) const { return (const T*)((unsigned char*)data + (size_t)w * y * elemsize); }

    template<typename T>
    inline T* row_c(int y) { return (T*)((unsigned char*)data + (size_t)w * y * c * elemsize); }
    template<typename T>
    inline const T* row_c(int y) const { return (const T*)((unsigned char*)data + (size_t)w * y * c * elemsize); }

    // range reference
    ImMat channel_range(int c, int channels);
    const ImMat channel_range(int c, int channels) const;
    ImMat row_range(int y, int rows);
    const ImMat row_range(int y, int rows) const;
    ImMat range(int x, int n);
    const ImMat range(int x, int n) const;

    // access raw data
    template<typename T> operator T*();
    template<typename T> operator const T*() const;

    // access element data
    template<typename _Tp> _Tp& at(int i=0) 
    {
        assert(device == IM_DD_CPU && dims == 1);
        return *(_Tp*)((unsigned char*)data + i * elemsize); 
    };
    template<typename _Tp> const _Tp& at(int i=0) const 
    {
        assert(device == IM_DD_CPU && dims == 1);
        return *(const _Tp*)((unsigned char*)data + i * elemsize); 
    };
    template<typename _Tp> _Tp& at(int x, int y) 
    {
        assert(device == IM_DD_CPU && dims == 2);
        return *(_Tp*)((unsigned char*)data + (y * w + x) * elemsize); 
    };
    template<typename _Tp> const _Tp& at(int x, int y) const 
    {
        assert(device == IM_DD_CPU && dims == 2);
        return *(const _Tp*)((unsigned char*)data + (y * w + x) * elemsize); 
    };
    template<typename _Tp> _Tp& at(int x, int y, int _c) 
    {
        assert(device == IM_DD_CPU && dims == 3);
        if (elempack == 1)
            return *(_Tp*)((unsigned char*)data + _c * cstep * elemsize + (y * w + x) * elemsize); 
        else
            return *(_Tp*)((unsigned char*)data + (y * w + x) * elemsize * c + _c); 
    };
    template<typename _Tp> const _Tp& at(int x, int y, int _c) const 
    {
        assert(device == IM_DD_CPU && dims == 3);
        if (elempack == 1)
            return *(const _Tp*)((unsigned char*)data + _c * cstep * elemsize + (y * w + x) * elemsize);
        else
            return *(const _Tp*)((unsigned char*)data + (y * w + x) * elemsize * c + _c); 
    };


    // convenient access float vec element
    float& operator[](size_t i);
    const float& operator[](size_t i) const;

    // pointer to the data
    void* data;

    // element size in bytes
    // 4 = float32/int32
    // 2 = float16
    // 1 = int8/uint8
    // 0 = empty
    size_t elemsize;

    // packed count inside element
    // c/1-h-w-1  h/1-w-1  w/1-1  scalar
    // c/4-h-w-4  h/4-w-4  w/4-4  sse/neon
    // c/8-h-w-8  h/8-w-8  w/8-8  avx/fp16
    int elempack;

    // the allocator
    Allocator* allocator;

    // the dimension rank
    int dims;

    int w;
    int h;
    int c;

    size_t cstep;

    // data device
    // 0 = cpu
    // 1 = vulkan
    // 2 = cuda
    ImDataDevice device;

    // device number
    // 0 = cpu
    // 0 - n = gpu index
    int device_number;

    // time stamp
    double time_stamp;

    // duration
    double duration;

    // audio sample rate
    // video frame rate
    Rational rate;

    // depth
    // 8~16 for int 32 for float
    int depth;

    // type
    // 0 = INT8/UINT8
    // 1 = INT16/UINT16
    // 2 = INT32/UINT32
    // 3 = INT64/UINT64
    // 4 = FLOAT16
    // 5 = FLOAT32
    // 6 = FLOAT64
    ImDataType type;

    // color
    // 0 = SRGB
    // 1 = BT601
    // 2 = BT709
    // 3 = BT2020
    ImColorSpace color_space;

    // format
    //  0 = GRAY
    //  1 = BGR
    //  2 = ABGR
    //  3 = BGRA
    //  4 = RGB
    //  5 = ARGB
    //  6 = RGBA
    //  7 = YUV420
    //  8 = YUV422
    //  9 = YUV444
    // 10 = YUVA
    // 11 = NV12
    // 12 = P010LE
    ImColorFormat color_format;

    // range
    // 0 = FULL_RANGE
    // 1 = NARROW_RANGE
    ImColorRange color_range;

    // flags, see define IM_MAT_FLAGS_XXX
    int flags;

    // ordination, see enum Ordination, default is NCWH
    Ordination ord;

protected:
    virtual void allocate_buffer();

    class RefCount
    {
    public:
        bool addref()
        {
            std::lock_guard<std::mutex> lk(l);
            if (c > 0)
            {
                c++;
                return true;
            }
            return false;
        }

        bool relref()
        {
            std::lock_guard<std::mutex> lk(l);
            if (c > 0)
            {
                c--;
                if (c == 0)
                    return true;
            }
            return false;
        }
    private:
        std::mutex l;
        unsigned int c{1};
    };

    // pointer to the reference counter
    // when points to user-allocated data, the pointer is NULL
    std::shared_ptr<RefCount> refcount;
};

//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
// ImMat class
//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
inline ImMat::ImMat()
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    type = IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    depth = 32;
    ord = ORD_NCWH;
    rate = {0, 0};
}

inline ImMat::ImMat(int _w, size_t _elemsize, Allocator* _allocator)
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    create(_w, _elemsize, _allocator);
}

inline ImMat::ImMat(int _w, int _h, size_t _elemsize, Allocator* _allocator)
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    create(_w, _h, _elemsize, _allocator);
}

inline ImMat::ImMat(int _w, int _h, int _c, size_t _elemsize, Allocator* _allocator)
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    create(_w, _h, _c, _elemsize, _allocator);
}

inline ImMat::ImMat(int _w, size_t _elemsize, int _elempack, Allocator* _allocator)
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    create(_w, _elemsize, _elempack, _allocator);
}

inline ImMat::ImMat(int _w, int _h, size_t _elemsize, int _elempack, Allocator* _allocator)
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    create(_w, _h, _elemsize, _elempack, _allocator);
}

inline ImMat::ImMat(int _w, int _h, int _c, size_t _elemsize, int _elempack, Allocator* _allocator)
    : data(0), device(IM_DD_CPU), device_number(-1), elemsize(0), elempack(0), allocator(0), dims(0), w(0), h(0), c(0), cstep(0), time_stamp(NAN), duration(NAN)
{
    create(_w, _h, _c, _elemsize, _elempack, _allocator);
}

inline ImMat::ImMat(const ImMat& m)
    : data(m.data), device(m.device), device_number(m.device_number), refcount(m.refcount), elemsize(m.elemsize), elempack(m.elempack), allocator(m.allocator), dims(m.dims), w(m.w), h(m.h), c(m.c), cstep(m.cstep), time_stamp(m.time_stamp), duration(m.duration)
{
    cstep = m.cstep;
    type = m.type;
    color_format = m.color_format;
    color_space = m.color_space;
    color_range = m.color_range;
    flags = m.flags;
    rate = m.rate;
    depth = m.depth;
    ord = m.ord;

    if (refcount && !refcount->addref())
    {
        // if argument 'm' is already released, then create an empty ImMat.
        refcount = nullptr;
        data = nullptr;
        *this = ImMat();
    }
}

inline ImMat::ImMat(int _w, void* _data, size_t _elemsize, Allocator* _allocator)
    : data(_data), device(IM_DD_CPU), device_number(-1), elemsize(_elemsize), elempack(1), allocator(_allocator), dims(1), w(_w), h(1), c(1), time_stamp(NAN), duration(NAN)
{
    cstep = w;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
}

inline ImMat::ImMat(int _w, int _h, void* _data, size_t _elemsize, Allocator* _allocator)
    : data(_data), device(IM_DD_CPU), device_number(-1), elemsize(_elemsize), elempack(1), allocator(_allocator), dims(2), w(_w), h(_h), c(1), time_stamp(NAN), duration(NAN)
{
    cstep = (size_t)w * h;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
}

inline ImMat::ImMat(int _w, int _h, int _c, void* _data, size_t _elemsize, Allocator* _allocator)
    : data(_data), device(IM_DD_CPU), device_number(-1), elemsize(_elemsize), elempack(1), allocator(_allocator), dims(3), w(_w), h(_h), c(_c), time_stamp(NAN), duration(NAN)
{
    cstep = Im_AlignSize((size_t)w * h * elemsize, 16) / elemsize;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = c == 1 ? IM_CF_GRAY : c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
}

inline ImMat::ImMat(int _w, void* _data, size_t _elemsize, int _elempack, Allocator* _allocator)
    : data(_data), device(IM_DD_CPU), device_number(-1), elemsize(_elemsize), elempack(_elempack), allocator(_allocator), dims(1), w(_w), h(1), c(1), time_stamp(NAN), duration(NAN)
{
    cstep = w;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
}

inline ImMat::ImMat(int _w, int _h, void* _data, size_t _elemsize, int _elempack, Allocator* _allocator)
    : data(_data), device(IM_DD_CPU), device_number(-1), elemsize(_elemsize), elempack(_elempack), allocator(_allocator), dims(2), w(_w), h(_h), c(1), time_stamp(NAN), duration(NAN)
{
    cstep = (size_t)w * h;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
}

inline ImMat::ImMat(int _w, int _h, int _c, void* _data, size_t _elemsize, int _elempack, Allocator* _allocator)
    : data(_data), device(IM_DD_CPU), device_number(-1), elemsize(_elemsize), elempack(_elempack), allocator(_allocator), dims(3), w(_w), h(_h), c(_c), time_stamp(NAN), duration(NAN)
{
    cstep = Im_AlignSize((size_t)w * h * elemsize, 16) / elemsize;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = c == 1 ? IM_CF_GRAY : c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = _elempack == _elemsize * _c ?  ORD_NWHC : ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
}

inline ImMat::~ImMat()
{
    release();
}

inline ImMat& ImMat::operator=(const ImMat& m)
{
    if (this == &m)
        return *this;

    if (m.refcount && !m.refcount->addref())
        // if argument 'm' is already released, then do nothing
        return *this;

    release();

    data = m.data;
    refcount = m.refcount;
    elemsize = m.elemsize;
    elempack = m.elempack;
    allocator = m.allocator;

    dims = m.dims;
    w = m.w;
    h = m.h;
    c = m.c;

    cstep = m.cstep;

    type = m.type;
    color_space = m.color_space;
    color_format = m.color_format;
    color_range = m.color_range;
    flags = m.flags;
    rate = m.rate;
    depth = m.depth;
    ord = m.ord;

    device = m.device;
    device_number = m.device_number;
    time_stamp = m.time_stamp;
    duration = m.duration;
    return *this;
}

inline void ImMat::allocate_buffer()
{
    size_t totalsize = Im_AlignSize(total() * elemsize, 4);

    if (allocator)
        data = allocator->fastMalloc(totalsize, device);
    else
        data = Im_FastMalloc(totalsize);
    if (!data)
        return;

    refcount = std::make_shared<RefCount>();
}

inline void ImMat::create(int _w, size_t _elemsize, Allocator* _allocator)
{
    if (dims == 1 && w == _w && elemsize == _elemsize && elempack == 1 && allocator == _allocator)
        return;

    release();

    elemsize = _elemsize;
    elempack = 1;
    allocator = _allocator;

    dims = 1;
    w = _w;
    h = 1;
    c = 1;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
    time_stamp = NAN;
    duration = NAN;

    cstep = w;

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create(int _w, int _h, size_t _elemsize, Allocator* _allocator)
{
    if (dims == 2 && w == _w && h == _h && elemsize == _elemsize && elempack == 1 && allocator == _allocator)
        return;

    release();

    elemsize = _elemsize;
    elempack = 1;
    allocator = _allocator;

    dims = 2;
    w = _w;
    h = _h;
    c = 1;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
    time_stamp = NAN;
    duration = NAN;

    cstep = (size_t)w * h;

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create(int _w, int _h, int _c, size_t _elemsize, Allocator* _allocator)
{
    if (dims == 3 && w == _w && h == _h && c == _c && elemsize == _elemsize && elempack == 1 && allocator == _allocator)
        return;

    release();

    elemsize = _elemsize;
    elempack = 1;
    allocator = _allocator;

    dims = 3;
    w = _w;
    h = _h;
    c = _c;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = c == 1 ? IM_CF_GRAY : c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
    time_stamp = NAN;
    duration = NAN;

    cstep = Im_AlignSize((size_t)w * h * elemsize, 16) / elemsize;

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create(int _w, size_t _elemsize, int _elempack, Allocator* _allocator)
{
    if (dims == 1 && w == _w && elemsize == _elemsize && elempack == _elempack && allocator == _allocator)
        return;

    release();

    elemsize = _elemsize;
    elempack = _elempack;
    allocator = _allocator;

    dims = 1;
    w = _w;
    h = 1;
    c = 1;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
    time_stamp = NAN;
    duration = NAN;

    cstep = w;

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create(int _w, int _h, size_t _elemsize, int _elempack, Allocator* _allocator)
{
    if (dims == 2 && w == _w && h == _h && elemsize == _elemsize && elempack == _elempack && allocator == _allocator)
        return;

    release();

    elemsize = _elemsize;
    elempack = _elempack;
    allocator = _allocator;

    dims = 2;
    w = _w;
    h = _h;
    c = 1;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
    time_stamp = NAN;
    duration = NAN;

    cstep = (size_t)w * h;

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create(int _w, int _h, int _c, size_t _elemsize, int _elempack, Allocator* _allocator)
{
    if (dims == 3 && w == _w && h == _h && c == _c && elemsize == _elemsize && elempack == _elempack && allocator == _allocator)
        return;

    release();

    elemsize = _elemsize;
    elempack = _elempack;
    allocator = _allocator;

    dims = 3;
    w = _w;
    h = _h;
    c = _c;
    type = _elemsize == 1 ? IM_DT_INT8 : _elemsize == 2 ? IM_DT_INT16 : IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = c == 1 ? IM_CF_GRAY : c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = _elempack == _elemsize * _c ?  ORD_NWHC : ORD_NCWH;
    depth = _elemsize == 1 ? 8 : _elemsize == 2 ? 16 : 32;
    time_stamp = NAN;
    duration = NAN;

    cstep = Im_AlignSize((size_t)w * h * elemsize, 16) / elemsize;

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create_type(int _w, ImDataType _t, Allocator* _allocator)
{
    if (dims == 1 && w == _w && elempack == 1 && type == _t && allocator == _allocator)
        return;

    release();

    elemsize = IM_ESIZE(_t);
    elempack = 1;
    allocator = _allocator;

    dims = 1;
    w = _w;
    h = 1;
    c = 1;

    cstep = w;
    type = _t;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    time_stamp = NAN;
    duration = NAN;
    depth = IM_DEPTH(_t);

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create_type(int _w, int _h, ImDataType _t, Allocator* _allocator)
{
    if (dims == 2 && w == _w && h == _h && elempack == 1 && type == _t && allocator == _allocator)
        return;

    release();

    elemsize = IM_ESIZE(_t);
    elempack = 1;
    allocator = _allocator;

    dims = 2;
    w = _w;
    h = _h;
    c = 1;

    cstep = (size_t)w * h;
    type = _t;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    time_stamp = NAN;
    duration = NAN;
    depth = IM_DEPTH(_t);

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create_type(int _w, int _h, int _c, ImDataType _t, Allocator* _allocator)
{
    if (dims == 3 && w == _w && h == _h && c == _c && elempack == 1 && type == _t && allocator == _allocator)
        return;

    release();

    elemsize = IM_ESIZE(_t);
    elempack = 1;
    allocator = _allocator;

    dims = 3;
    w = _w;
    h = _h;
    c = _c;

    cstep = Im_AlignSize((size_t)w * h * elemsize, 16) / elemsize;
    type = _t;
    color_space = IM_CS_SRGB;
    color_format = c == 1 ? IM_CF_GRAY : c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    time_stamp = NAN;
    duration = NAN;
    depth = IM_DEPTH(_t);

    if (total() > 0)
        allocate_buffer();
}

inline void ImMat::create_type(int _w, void* _data, ImDataType _t, Allocator* _allocator)
{
    if (dims == 1 && w == _w && elempack == 1 && type == _t && allocator == _allocator)
        return;

    release();

    elemsize = IM_ESIZE(_t);
    elempack = 1;
    allocator = _allocator;
    refcount = nullptr;

    dims = 1;
    w = _w;
    h = 1;
    c = 1;

    cstep = w;
    type = _t;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    time_stamp = NAN;
    duration = NAN;
    depth = IM_DEPTH(_t);
    data = _data;
}

inline void ImMat::create_type(int _w, int _h, void* _data, ImDataType _t, Allocator* _allocator)
{
    if (dims == 2 && w == _w && h == _h && elempack == 1 && type == _t && allocator == _allocator)
        return;

    release();

    elemsize = IM_ESIZE(_t);
    elempack = 1;
    allocator = _allocator;
    refcount = nullptr;

    dims = 2;
    w = _w;
    h = _h;
    c = 1;

    cstep = (size_t)w * h;
    type = _t;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_GRAY;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    time_stamp = NAN;
    duration = NAN;
    depth = IM_DEPTH(_t);
    data = _data;
}

inline void ImMat::create_type(int _w, int _h, int _c, void* _data, ImDataType _t, Allocator* _allocator)
{
    if (dims == 3 && w == _w && h == _h && c == _c && elempack == 1 && type == _t && allocator == _allocator)
        return;

    release();

    elemsize = IM_ESIZE(_t);
    elempack = 1;
    allocator = _allocator;
    refcount = nullptr;

    dims = 3;
    w = _w;
    h = _h;
    c = _c;

    cstep = Im_AlignSize((size_t)w * h * elemsize, 4) / elemsize;
    type = _t;
    color_space = IM_CS_SRGB;
    color_format = c == 1 ? IM_CF_GRAY : c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    time_stamp = NAN;
    duration = NAN;
    depth = IM_DEPTH(_t);
    data = _data;
}

inline void ImMat::create_like(const ImMat& m, Allocator* _allocator)
{
    int _dims = m.dims;
    if (_dims == 1)
        create(m.w, m.elemsize, m.elempack, _allocator);
    if (_dims == 2)
        create(m.w, m.h, m.elemsize, m.elempack, _allocator);
    if (_dims == 3)
        create(m.w, m.h, m.c, m.elemsize, m.elempack, _allocator);
    type = m.type;
    color_space = m.color_space;
    color_format = m.color_format;
    color_range = m.color_range;
    flags = m.flags;
    rate = m.rate;
    ord = m.ord;
    depth = m.depth;
    time_stamp = m.time_stamp;
    duration = m.duration;
}

inline void ImMat::release()
{
    if (refcount && refcount->relref())
    {
        if (allocator && data)
            allocator->fastFree(data, device);
        else if (data)
            Im_FastFree(data);
    }
    data = 0;
    refcount = nullptr;

    elemsize = 0;
    elempack = 0;

    dims = 0;
    w = 0;
    h = 0;
    c = 0;

    cstep = 0;

    type = IM_DT_FLOAT32;
    color_space = IM_CS_SRGB;
    color_format = IM_CF_ABGR;
    color_range = IM_CR_FULL_RANGE;
    flags = IM_MAT_FLAGS_NONE;
    rate = {0, 0};
    ord = ORD_NCWH;
    depth = 32;
    time_stamp = NAN;
    duration = NAN;
    device = IM_DD_CPU;
    device_number = -1;
}

inline bool ImMat::empty() const
{
    return data == 0 || total() == 0;
}

inline size_t ImMat::total() const
{
    return cstep * c;
}

inline int ImMat::elembits() const
{
    return elempack ? static_cast<int>(elemsize * 8) / elempack : 0;
}

inline ImMat ImMat::shape() const
{
    if (dims == 1)
        return ImMat(w * elempack, (void*)0);
    if (dims == 2)
        return ImMat(w, h * elempack, (void*)0);
    if (dims == 3)
        return ImMat(w, h, c * elempack, (void*)0);

    return ImMat();
}

inline ImMat ImMat::channel(int _c)
{
    ImMat m(w, h, (unsigned char*)data + cstep * _c * elemsize, elemsize, elempack, allocator);
    m.dims = dims - 1;
    return m;
}

inline const ImMat ImMat::channel(int _c) const
{
    ImMat m(w, h, (unsigned char*)data + cstep * _c * elemsize, elemsize, elempack, allocator);
    m.dims = dims - 1;
    return m;
}

inline ImMat ImMat::channel_range(int _c, int channels)
{
    return ImMat(w, h, channels, (unsigned char*)data + cstep * _c * elemsize, elemsize, elempack, allocator);
}

inline const ImMat ImMat::channel_range(int _c, int channels) const
{
    return ImMat(w, h, channels, (unsigned char*)data + cstep * _c * elemsize, elemsize, elempack, allocator);
}

inline ImMat ImMat::row_range(int y, int rows)
{
    return ImMat(w, rows, (unsigned char*)data + (size_t)w * y * elemsize, elemsize, elempack, allocator);
}

inline const ImMat ImMat::row_range(int y, int rows) const
{
    return ImMat(w, rows, (unsigned char*)data + (size_t)w * y * elemsize, elemsize, elempack, allocator);
}

inline ImMat ImMat::range(int x, int n)
{
    return ImMat(n, (unsigned char*)data + x * elemsize, elemsize, elempack, allocator);
}

inline const ImMat ImMat::range(int x, int n) const
{
    return ImMat(n, (unsigned char*)data + x * elemsize, elemsize, elempack, allocator);
}

template<typename T>
inline ImMat::operator T*()
{
    return (T*)data;
}

template<typename T>
inline ImMat::operator const T*() const
{
    return (const T*)data;
}

inline float& ImMat::operator[](size_t i)
{
    return ((float*)data)[i];
}

inline const float& ImMat::operator[](size_t i) const
{
    return ((const float*)data)[i];
}

template<typename T>
inline void ImMat::fill(T _v)
{
    int size = (int)total();
    T* ptr = (T*)data;
    for (int i = 0; i < size; i++)
    {
        ptr[i] = _v;
    }
}

inline ImMat ImMat::clone(Allocator* _allocator) const
{
    if (empty())
        return ImMat();

    ImMat m;
    if (dims == 1)
        m.create(w, elemsize, elempack, _allocator);
    else if (dims == 2)
        m.create(w, h, elemsize, elempack, _allocator);
    else if (dims == 3)
        m.create(w, h, c, elemsize, elempack, _allocator);

    if (total() > 0)
    {
        if (cstep == m.cstep)
            memcpy(m.data, data, total() * elemsize);
        else
        {
            // copy by channel for differnet cstep
            size_t size = (size_t)w * h * elemsize;
            for (int i = 0; i < c; i++)
            {
                memcpy(m.channel(i), channel(i), size);
            }
        }
    }
    m.color_format = color_format;
    m.color_range = color_range;
    m.color_space = color_space;
    m.type = type;
    m.time_stamp = time_stamp;
    m.duration = duration;
    m.flags = flags;
    m.depth = depth;
    m.rate = rate;
    m.ord = ord;
    return m;
}

inline void ImMat::clone_from(const ImMat& mat, Allocator* allocator)
{
    *this = mat.clone(allocator);
}

inline ImMat ImMat::reshape(int _w, Allocator* _allocator) const
{
    if (w * h * c != _w)
        return ImMat();

    if (dims == 3 && cstep != (size_t)w * h)
    {
        ImMat m;
        m.create(_w, elemsize, elempack, _allocator);
        if (!m.data)
            return m;
        // flatten
        for (int i = 0; i < c; i++)
        {
            const void* ptr = (unsigned char*)data + i * cstep * elemsize;
            void* mptr = (unsigned char*)m.data + (size_t)i * w * h * elemsize;
            memcpy(mptr, ptr, (size_t)w * h * elemsize);
        }

        return m;
    }

    ImMat m = *this;

    m.dims = 1;
    m.w = _w;
    m.h = 1;
    m.c = 1;

    m.cstep = _w;
    m.color_format = IM_CF_GRAY;

    m.time_stamp = time_stamp;
    m.duration = duration;
    m.flags = flags;
    m.rate = rate;
    m.ord = ord;

    return m;
}

inline ImMat ImMat::reshape(int _w, int _h, Allocator* _allocator) const
{
    if (w * h * c != _w * _h)
        return ImMat();

    if (dims == 3 && cstep != (size_t)w * h)
    {
        ImMat m;
        m.create(_w, _h, elemsize, elempack, _allocator);

        // flatten
        for (int i = 0; i < c; i++)
        {
            const void* ptr = (unsigned char*)data + i * cstep * elemsize;
            void* mptr = (unsigned char*)m.data + (size_t)i * w * h * elemsize;
            memcpy(mptr, ptr, (size_t)w * h * elemsize);
        }

        return m;
    }

    ImMat m = *this;

    m.dims = 2;
    m.w = _w;
    m.h = _h;
    m.c = 1;
    m.color_format = IM_CF_GRAY;

    m.cstep = (size_t)_w * _h;

    m.time_stamp = time_stamp;
    m.duration = duration;
    m.flags = flags;
    m.rate = rate;
    m.ord = ord;

    return m;
}

inline ImMat ImMat::reshape(int _w, int _h, int _c, Allocator* _allocator) const
{
    if (w * h * c != _w * _h * _c)
        return ImMat();

    if (dims < 3)
    {
        if ((size_t)_w * _h != Im_AlignSize((size_t)_w * _h * elemsize, 16) / elemsize)
        {
            ImMat m;
            m.create(_w, _h, _c, elemsize, elempack, _allocator);

            // align channel
            for (int i = 0; i < _c; i++)
            {
                const void* ptr = (unsigned char*)data + (size_t)i * _w * _h * elemsize;
                void* mptr = (unsigned char*)m.data + i * m.cstep * m.elemsize;
                memcpy(mptr, ptr, (size_t)_w * _h * elemsize);
            }

            return m;
        }
    }
    else if (c != _c)
    {
        // flatten and then align
        ImMat tmp = reshape(_w * _h * _c, _allocator);
        return tmp.reshape(_w, _h, _c, _allocator);
    }

    ImMat m = *this;

    m.dims = 3;
    m.w = _w;
    m.h = _h;
    m.c = _c;
    m.color_format = _c == 1 ? IM_CF_GRAY : _c == 3 ? IM_CF_BGR : IM_CF_ABGR;
    m.cstep = Im_AlignSize((size_t)_w * _h * elemsize, 16) / elemsize;

    m.time_stamp = time_stamp;
    m.duration = duration;
    m.flags = flags;
    m.rate = rate;
    m.ord = ord;

    return m;
}

inline ImMat ImMat::t() const
{
    assert(device == IM_DD_CPU);
    assert(total() > 0);
    if (dims == 1)
    {
        ImMat m;
        m.create_type(w, type, allocator);
        if (!m.data)
            return m;
        const void* ptr = (unsigned char*)data;
        void* mptr = (unsigned char*)m.data;
        memcpy(mptr, ptr, (size_t)w * h * elemsize);
        m.w = 1;
        m.h = w;

        return m;
    }
    else if (dims == 2)
    {
        ImMat m;
        m.create_type(h, w, type, allocator);
        if (!m.data)
            return m;
        for (int _h = 0; _h < h; _h++)
        {
            for (int _w = 0; _w < w; _w++)
            {
                switch (type)
                {
                    case IM_DT_INT8:    m.at< int8_t>(_h, _w) = this->at< int8_t>(_w, _h); break;
                    case IM_DT_INT16:   m.at<int16_t>(_h, _w) = this->at<int16_t>(_w, _h); break;
                    case IM_DT_INT32:   m.at<int32_t>(_h, _w) = this->at<int32_t>(_w, _h); break;
                    case IM_DT_INT64:   m.at<int64_t>(_h, _w) = this->at<int64_t>(_w, _h); break;
                    case IM_DT_FLOAT32: m.at<float>  (_h, _w) = this->at<float>  (_w, _h); break;
                    case IM_DT_FLOAT64: m.at<double> (_h, _w) = this->at<double> (_w, _h); break;
                    case IM_DT_FLOAT16: m.at<int16_t>(_h, _w) = this->at<int16_t>(_w, _h); break;
                    default: break;
                }
            }
        }
        return m;
    }
    else if (dims == 3)
    {
        ImMat m;
        m.create_type(h, w, c, type, allocator);
        if (!m.data)
            return m;
        
        for (int _c = 0; _c < c; _c++)
        {
            for (int _h = 0; _h < h; _h++)
            {
                for (int _w = 0; _w < w; _w++)
                {
                    switch (type)
                    {
                        case IM_DT_INT8:    m.at< int8_t>(_h, _w, _c) = this->at< int8_t>(_w, _h, _c); break;
                        case IM_DT_INT16:   m.at<int16_t>(_h, _w, _c) = this->at<int16_t>(_w, _h, _c); break;
                        case IM_DT_INT32:   m.at<int32_t>(_h, _w, _c) = this->at<int32_t>(_w, _h, _c); break;
                        case IM_DT_INT64:   m.at<int64_t>(_h, _w, _c) = this->at<int64_t>(_w, _h, _c); break;
                        case IM_DT_FLOAT32: m.at<float>  (_h, _w, _c) = this->at<float>  (_w, _h, _c); break;
                        case IM_DT_FLOAT64: m.at<double> (_h, _w, _c) = this->at<double> (_w, _h, _c); break;
                        case IM_DT_FLOAT16: m.at<int16_t>(_h, _w, _c) = this->at<int16_t>(_w, _h, _c); break;
                        default: break;
                    }
                }
            }
        }
        return m;
    }
    return ImMat();
}

// invert
template<typename T>
inline ImMat ImMat::inv() const
{
    assert(device == IM_DD_CPU);
    assert(dims == 2 && w == h);
    assert(total() > 0);
    ImGui::ImMat inverse_mat, tmp_mat;
    inverse_mat.create_type(w, h, type);
    tmp_mat.clone_from(*this);
    inverse_mat.eye((T)1);
    T max, temp, k;
    for (int i = 0; i < w; i++)
	{
        max = tmp_mat.at<T>(i, i);
        k = i;
		for (int j = i + 1; j < w; j++)
		{
            if (std::abs(tmp_mat.at<T>(j, i)) > std::abs(max))
			{
				max = tmp_mat.at<T>(j, i);
				k = j;
			}
        }
        if (k != i)
		{
			for (int j = 0; j < w; j++)
			{
				temp = tmp_mat.at<T>(i, j);
				tmp_mat.at<T>(i, j) = tmp_mat.at<T>(k, j);
				tmp_mat.at<T>(k, j) = temp;

				temp = inverse_mat.at<T>(i, j);
				inverse_mat.at<T>(i, j) = inverse_mat.at<T>(k, j);
				inverse_mat.at<T>(k, j) = temp;
			}
		}
        if (tmp_mat.at<T>(i, i) == 0)
		{
            // There is no inverse matrix
            inverse_mat.fill(0);
            return inverse_mat;
        }
        temp = tmp_mat.at<T>(i, i);
		for (int j = 0; j < w; j++)
		{
			tmp_mat.at<T>(i, j) = tmp_mat.at<T>(i, j) / temp;
			inverse_mat.at<T>(i, j) = inverse_mat.at<T>(i, j) / temp;
		}
        for (int j = 0; j < w; j++)
		{
			if (j != i)
			{
				temp = tmp_mat.at<T>(j, i);
				for (int l = 0; l < w; l++)
				{
					tmp_mat.at<T>(j, l) = tmp_mat.at<T>(j, l) - tmp_mat.at<T>(i, l) * temp;
					inverse_mat.at<T>(j, l) = inverse_mat.at<T>(j, l) - inverse_mat.at<T>(i, l) * temp;
				}
			}
		}
    }
    return inverse_mat;
}

// eye
template<typename T> 
inline ImMat& ImMat::eye(T scale)
{
    assert(device == IM_DD_CPU);
    assert(total() > 0);
    if (dims == 1)
    {
        switch (type)
        {
            case IM_DT_INT8:    this->at< int8_t>(0) = scale; break;
            case IM_DT_INT16:   this->at<int16_t>(0) = scale; break;
            case IM_DT_INT32:   this->at<int32_t>(0) = scale; break;
            case IM_DT_INT64:   this->at<int64_t>(0) = scale; break;
            case IM_DT_FLOAT32: this->at<float>  (0) = scale; break;
            case IM_DT_FLOAT64: this->at<double> (0) = scale; break;
            case IM_DT_FLOAT16: this->at<uint16_t>(0) = im_float32_to_float16(scale); break;
            default: break;
        }
    }
    else if (dims == 2)
    {
        for (int _h = 0; _h < h; _h++)
        {
            for (int _w = 0; _w < w; _w++)
            {
                switch (type)
                {
                    case IM_DT_INT8:    this->at< int8_t>(_w, _h) = _w == _h ? scale : 0; break;
                    case IM_DT_INT16:   this->at<int16_t>(_w, _h) = _w == _h ? scale : 0; break;
                    case IM_DT_INT32:   this->at<int32_t>(_w, _h) = _w == _h ? scale : 0; break;
                    case IM_DT_INT64:   this->at<int64_t>(_w, _h) = _w == _h ? scale : 0; break;
                    case IM_DT_FLOAT32: this->at<float>  (_w, _h) = _w == _h ? scale : 0; break;
                    case IM_DT_FLOAT64: this->at<double> (_w, _h) = _w == _h ? scale : 0; break;
                    case IM_DT_FLOAT16: this->at<uint16_t>(_w, _h)= _w == _h ? im_float32_to_float16(scale) : 0; break;
                    default: break;
                }
            }
        }
    }
    else if (dims == 3)
    {
        for (int _c = 0; _c < c; _c++)
        {
            for (int _h = 0; _h < h; _h++)
            {
                for (int _w = 0; _w < w; _w++)
                {
                    switch (type)
                    {
                        case IM_DT_INT8:    this->at< int8_t>(_w, _h, _c) = _w == _h ? scale : 0; break;
                        case IM_DT_INT16:   this->at<int16_t>(_w, _h, _c) = _w == _h ? scale : 0; break;
                        case IM_DT_INT32:   this->at<int32_t>(_w, _h, _c) = _w == _h ? scale : 0; break;
                        case IM_DT_INT64:   this->at<int64_t>(_w, _h, _c) = _w == _h ? scale : 0; break;
                        case IM_DT_FLOAT32: this->at<float>  (_w, _h, _c) = _w == _h ? scale : 0; break;
                        case IM_DT_FLOAT64: this->at<double> (_w, _h, _c) = _w == _h ? scale : 0; break;
                        case IM_DT_FLOAT16: this->at<uint16_t>(_w, _h, _c)= _w == _h ? im_float32_to_float16(scale) : 0; break;
                        default: break;
                    }
                }
            }
        }
    }
    return *this;
}

// rand
template<typename T> 
inline ImMat& ImMat::randn(T mean, T stddev)
{
    assert(device == IM_DD_CPU);
    assert(total() > 0);

    unsigned int seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::default_random_engine gen(seed);
    std::normal_distribution<T> dis(mean, stddev);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = (int8_t)dis(gen); } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = (int16_t)dis(gen); } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = (int32_t)dis(gen); } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = (int64_t)dis(gen); } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = (float)dis(gen); } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = (double)dis(gen); } break;
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16((float)dis(gen)); } break;
            default: break;
        }
    }
    return *this;
}

// clip
template<typename T> ImMat& ImMat::clip(T v_min, T v_max)
{
    assert(device == IM_DD_CPU);
    assert(total() > 0);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    {   if (((int8_t *) this->data)[i] < (int8_t)  v_min) ((int8_t *) this->data)[i] = (int8_t) v_min;
                                    if (((int8_t *) this->data)[i] > (int8_t)  v_max) ((int8_t *) this->data)[i] = (int8_t) v_max; } break;
            case IM_DT_INT16:   {   if (((int16_t *)this->data)[i] < (int16_t) v_min) ((int16_t *)this->data)[i]= (int16_t) v_min;
                                    if (((int16_t *)this->data)[i] > (int16_t) v_max) ((int16_t *)this->data)[i]= (int16_t) v_max; } break; 
            case IM_DT_INT32:   {   if (((int32_t *)this->data)[i] < (int32_t) v_min) ((int32_t *)this->data)[i]= (int32_t) v_min;
                                    if (((int32_t *)this->data)[i] > (int32_t) v_max) ((int32_t *)this->data)[i]= (int32_t) v_max; } break; 
            case IM_DT_INT64:   {   if (((int64_t *)this->data)[i] < (int64_t) v_min) ((int64_t *)this->data)[i]= (int64_t) v_min;
                                    if (((int64_t *)this->data)[i] > (int64_t) v_max) ((int64_t *)this->data)[i]= (int64_t) v_max; } break; 
            case IM_DT_FLOAT32: {   if (((float *)  this->data)[i] < (float)   v_min) ((float *)  this->data)[i]= (float)   v_min;
                                    if (((float *)  this->data)[i] > (float)   v_max) ((float *)  this->data)[i]= (float)   v_max; } break; 
            case IM_DT_FLOAT64: {   if (((double *) this->data)[i] < (double)  v_min) ((double *) this->data)[i]= (double)  v_min;
                                    if (((double *) this->data)[i] > (double)  v_max) ((double *) this->data)[i]= (double)  v_max; } break;
            case IM_DT_FLOAT16: {   if (im_float16_to_float32(((uint16_t *)this->data)[i]) < (float)v_min) ((uint16_t *)this->data)[i] = im_float32_to_float16((float)v_min);
                                    if (im_float16_to_float32(((uint16_t *)this->data)[i]) > (float)v_max) ((uint16_t *)this->data)[i] = im_float32_to_float16((float)v_max); } break;
            default: break;
        }
    }
    return *this;
}

// scalar add
template<typename T> 
inline ImMat ImMat::operator+ (T v)
{
    assert(device == IM_DD_CPU);
    ImMat m;
    m.create_like(*this);
    if (!m.data)
        return m;
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) m.data)[i] = ((int8_t *) this->data)[i] + static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] + static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] + static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] + static_cast<int64_t>(v); } break;
            case IM_DT_FLOAT32: { ((float *)  m.data)[i] = ((float *)  this->data)[i] + static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { ((double *) m.data)[i] = ((double *) this->data)[i] + static_cast<double> (v); } break;
            case IM_DT_FLOAT16: { ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) + static_cast<float>(v)); } break;
            default: break;
        }
    }
    return m;
}

template<typename T> 
inline ImMat& ImMat::operator+=(T v)
{
    assert(device == IM_DD_CPU);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] + static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] + static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] + static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] + static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] + static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] + static_cast<double> (v); } break;
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) + static_cast<float>(v)); } break;
            default: break;
        }
    }
    return *this;
}

// scalar sub
template<typename T> 
inline ImMat ImMat::operator- (T v)
{
    assert(device == IM_DD_CPU);
    ImMat m;
    m.create_like(*this);
    if (!m.data)
        return m;
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) m.data)[i] = ((int8_t *) this->data)[i] - static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] - static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] - static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] - static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { ((float *)  m.data)[i] = ((float *)  this->data)[i] - static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { ((double *) m.data)[i] = ((double *) this->data)[i] - static_cast<double> (v); } break;
            case IM_DT_FLOAT16: { ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) - static_cast<float>(v)); } break;
            default: break;
        }
    }

    return m;
}

template<typename T> 
inline ImMat& ImMat::operator-=(T v)
{
    assert(device == IM_DD_CPU);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] - static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] - static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] - static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] - static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] - static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] - static_cast<double> (v); } break;
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) - static_cast<float>(v)); } break;
            default: break;
        }
    }

    return *this;
}

// scalar mul
template<typename T> 
inline ImMat ImMat::operator* (T v)
{
    assert(device == IM_DD_CPU);
    ImMat m;
    m.create_like(*this);
    if (!m.data)
        return m;
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) m.data)[i] = ((int8_t *) this->data)[i] * static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] * static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] * static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] * static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { ((float *)  m.data)[i] = ((float *)  this->data)[i] * static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { ((double *) m.data)[i] = ((double *) this->data)[i] * static_cast<double> (v); } break;
            case IM_DT_FLOAT16: { ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) * static_cast<float>(v)); } break;
            default: break;
        }
    }

    return m;
}

template<typename T> 
inline ImMat& ImMat::operator*=(T v)
{
    assert(device == IM_DD_CPU);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] * static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] * static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] * static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] * static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] * static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] * static_cast<double> (v); } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) * static_cast<float>(v)); } break;
            default: break;
        }
    }

    return *this;
}

// scalar div
template<typename T> 
inline ImMat ImMat::operator/ (T v)
{
    assert(device == IM_DD_CPU);
    ImMat m;
    m.create_like(*this);
    if (!m.data)
        return m;
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { if (static_cast<int8_t> (v) != 0) ((int8_t *) m.data)[i] = ((int8_t *) this->data)[i] / static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { if (static_cast<int16_t>(v) != 0) ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] / static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { if (static_cast<int32_t>(v) != 0) ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] / static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { if (static_cast<int64_t>(v) != 0) ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] / static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { if (static_cast<float>  (v) != 0) ((float *)  m.data)[i] = ((float *)  this->data)[i] / static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { if (static_cast<double> (v) != 0) ((double *) m.data)[i] = ((double *) this->data)[i] / static_cast<double> (v); } break; 
            case IM_DT_FLOAT16: { if (static_cast<float>  (v) != 0) ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) / static_cast<float>(v)); } break;
            default: break;
        }
    }

    return m;
}

template<typename T> 
inline ImMat& ImMat::operator/=(T v)
{
    assert(device == IM_DD_CPU);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { if (static_cast<int8_t> (v) != 0) ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] / static_cast<int8_t> (v); } break;
            case IM_DT_INT16:   { if (static_cast<int16_t>(v) != 0) ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] / static_cast<int16_t>(v); } break; 
            case IM_DT_INT32:   { if (static_cast<int32_t>(v) != 0) ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] / static_cast<int32_t>(v); } break; 
            case IM_DT_INT64:   { if (static_cast<int64_t>(v) != 0) ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] / static_cast<int64_t>(v); } break; 
            case IM_DT_FLOAT32: { if (static_cast<float>  (v) != 0) ((float *)  this->data)[i] = ((float *)  this->data)[i] / static_cast<float>  (v); } break; 
            case IM_DT_FLOAT64: { if (static_cast<double> (v) != 0) ((double *) this->data)[i] = ((double *) this->data)[i] / static_cast<double> (v); } break; 
            case IM_DT_FLOAT16: { if (static_cast<float>  (v) != 0) ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) / static_cast<float>(v)); } break;
            default: break;
        }
    }

    return *this;
}

// mat add
inline ImMat ImMat::operator+(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    ImMat m;
    m.create_like(*this);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *)m.data)[i]  = ((int8_t *) this->data)[i] + ((int8_t *) mat.data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] + ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] + ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] + ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)m.data)[i]   = ((float *)  this->data)[i] + ((float *)  mat.data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *)m.data)[i]  = ((double *) this->data)[i] + ((double *) mat.data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) + 
                                                                                 im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return m;
}

inline ImMat& ImMat::operator+=(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] + ((int8_t *) mat.data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] + ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] + ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] + ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] + ((float *)  mat.data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] + ((double *) mat.data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) + 
                                                                                     im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return *this;
}

// mat sub
inline ImMat ImMat::operator-(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    ImMat m;
    m.create_like(*this);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *)m.data)[i]  = ((int8_t *)this->data)[i]  - ((int8_t *)mat.data) [i]; } break;
            case IM_DT_INT16:   { ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] - ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] - ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] - ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)m.data)[i]   = ((float *)this->data)[i]   - ((float *)mat.data)  [i]; } break; 
            case IM_DT_FLOAT64: { ((double *)m.data)[i]  = ((double *)this->data)[i]  - ((double *)mat.data) [i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) - 
                                                                                 im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return m;
}

inline ImMat& ImMat::operator-=(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *)this->data)[i]  - ((int8_t *) mat.data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] - ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] - ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] - ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)this->data)[i]   - ((float *)  mat.data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *)this->data)[i]  - ((double *) mat.data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) - 
                                                                                     im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return *this;
}

// mat div
inline ImMat ImMat::operator/(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    ImMat m;
    m.create_like(*this);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *)m.data)[i]  = ((int8_t *) this->data)[i] / ((int8_t *) mat.data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)m.data)[i] = ((int16_t *)this->data)[i] / ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)m.data)[i] = ((int32_t *)this->data)[i] / ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)m.data)[i] = ((int64_t *)this->data)[i] / ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)m.data)[i]   = ((float *)  this->data)[i] / ((float *)  mat.data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *)m.data)[i]  = ((double *) this->data)[i] / ((double *) mat.data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)m.data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) / 
                                                                                 im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return m;
}

inline ImMat& ImMat::operator/=(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] / ((int8_t *) mat.data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] / ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] / ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] / ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] / ((float *)  mat.data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] / ((double *) mat.data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) / 
                                                                                     im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return *this;
}

inline ImMat& ImMat::square()
{
    assert(device == IM_DD_CPU);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] * ((int8_t *) this->data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] * ((int16_t *)this->data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] * ((int32_t *)this->data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] * ((int64_t *)this->data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] * ((float *)  this->data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] * ((double *) this->data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) * 
                                                                                     im_float16_to_float32(((unsigned short *)this->data)[i])); } break;
            default: break;
        }
    }
    return *this;
}

// mat mul
inline ImMat& ImMat::mul(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(w == mat.w);
    assert(h == mat.h);
    assert(c == mat.c);
    assert(type == mat.type);
    #pragma omp parallel for num_threads(OMP_THREADS)
    for (int i = 0; i < total(); i++)
    {
        switch (type)
        {
            case IM_DT_INT8:    { ((int8_t *) this->data)[i] = ((int8_t *) this->data)[i] * ((int8_t *) mat.data)[i]; } break;
            case IM_DT_INT16:   { ((int16_t *)this->data)[i] = ((int16_t *)this->data)[i] * ((int16_t *)mat.data)[i]; } break; 
            case IM_DT_INT32:   { ((int32_t *)this->data)[i] = ((int32_t *)this->data)[i] * ((int32_t *)mat.data)[i]; } break; 
            case IM_DT_INT64:   { ((int64_t *)this->data)[i] = ((int64_t *)this->data)[i] * ((int64_t *)mat.data)[i]; } break; 
            case IM_DT_FLOAT32: { ((float *)  this->data)[i] = ((float *)  this->data)[i] * ((float *)  mat.data)[i]; } break; 
            case IM_DT_FLOAT64: { ((double *) this->data)[i] = ((double *) this->data)[i] * ((double *) mat.data)[i]; } break; 
            case IM_DT_FLOAT16: { ((uint16_t *)this->data)[i]= im_float32_to_float16(im_float16_to_float32(((unsigned short *)this->data)[i]) * 
                                                                                     im_float16_to_float32(((unsigned short *)mat.data)[i])); } break;
            default: break;
        }
    }
    return *this;
}

// mat dot mul
inline ImMat ImMat::operator*(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(dims == 2);
    assert(w == mat.h);
    ImMat m;
    m.create_type(mat.w, h, type, allocator);
    if (!m.data)
        return m;
    for (int i = 0; i < m.h; i++)
    {
        for (int j = 0; j < m.w; j++)
        {
            for (int k = 0; k < w; k++)
            {
                switch (type)
                {
                    case IM_DT_INT8:    m.at<int8_t> (j, i) += this->at<int8_t> (k, i) * mat.at<int8_t> (j, k); break;
                    case IM_DT_INT16:   m.at<int16_t>(j, i) += this->at<int16_t>(k, i) * mat.at<int16_t>(j, k); break;
                    case IM_DT_INT32:   m.at<int32_t>(j, i) += this->at<int32_t>(k, i) * mat.at<int32_t>(j, k); break;
                    case IM_DT_INT64:   m.at<int64_t>(j, i) += this->at<int64_t>(k, i) * mat.at<int64_t>(j, k); break;
                    case IM_DT_FLOAT32: m.at<float>  (j, i) += this->at<float>  (k, i) * mat.at<float>  (j, k); break;
                    case IM_DT_FLOAT64: m.at<double> (j, i) += this->at<double> (k, i) * mat.at<double> (j, k); break;
                    case IM_DT_FLOAT16: m.at<uint16_t>(j, i) = im_float32_to_float16(
                                                                im_float16_to_float32(m.at<uint16_t>(j, i)) + 
                                                                im_float16_to_float32(this->at<uint16_t>(k, i)) *
                                                                im_float16_to_float32(mat.at<uint16_t> (j, k))); break;
                    default: break;
                }
            }
        }
    }
    return m;
}

inline ImMat& ImMat::operator*=(const ImMat& mat)
{
    assert(device == IM_DD_CPU);
    assert(dims == 2);
    assert(w == mat.h);
    ImMat m;
    m.clone_from(*this);
    this->release();
    this->create_type(mat.w, m.h, m.type, allocator);
    for (int i = 0; i < h; i++)
    {
        for (int j = 0; j < w; j++)
        {
            for (int k = 0; k < m.w; k++)
            {
                switch (type)
                {
                    case IM_DT_INT8:    this->at<int8_t> (j, i) += m.at<int8_t> (k, i) * mat.at<int8_t> (j, k); break;
                    case IM_DT_INT16:   this->at<int16_t>(j, i) += m.at<int16_t>(k, i) * mat.at<int16_t>(j, k); break;
                    case IM_DT_INT32:   this->at<int32_t>(j, i) += m.at<int32_t>(k, i) * mat.at<int32_t>(j, k); break;
                    case IM_DT_INT64:   this->at<int64_t>(j, i) += m.at<int64_t>(k, i) * mat.at<int64_t>(j, k); break;
                    case IM_DT_FLOAT32: this->at<float>  (j, i) += m.at<float>  (k, i) * mat.at<float>  (j, k); break;
                    case IM_DT_FLOAT64: this->at<double> (j, i) += m.at<double> (k, i) * mat.at<double> (j, k); break;
                    case IM_DT_FLOAT16: this->at<uint16_t>(j, i) = im_float32_to_float16(
                                                                    im_float16_to_float32(this->at<uint16_t>(j, i)) + 
                                                                    im_float16_to_float32(m.at<uint16_t>(k, i)) *
                                                                    im_float16_to_float32(mat.at<uint16_t> (j, k))); break;
                    default: break;
                }
            }
        }
    }
    return *this;
}

inline void ImMat::clean(ImPixel color)
{
    assert(dims == 3);
    assert(c > 0);
    assert(data);
    switch (type)
    {
        case IM_DT_INT8:
        {
            uint8_t s_buf[4] = {(uint8_t)(color.r * UINT8_MAX), 
                                (uint8_t)(color.g * UINT8_MAX),
                                (uint8_t)(color.b * UINT8_MAX),
                                (uint8_t)(color.a * UINT8_MAX)};
            for (int i = 0; i < total() / c; i++)
            {
                memcpy((uint8_t*)data + i * elemsize * c, s_buf, c * elemsize);
            }
        }
        break;
        case IM_DT_INT16:
        {
            uint16_t s_buf[4] = {(uint16_t)(color.r * UINT16_MAX), 
                                 (uint16_t)(color.g * UINT16_MAX),
                                 (uint16_t)(color.b * UINT16_MAX),
                                 (uint16_t)(color.a * UINT16_MAX)};
            for (int i = 0; i < total() / c; i++)
            {
                memcpy((uint8_t*)data + i * elemsize * c, s_buf, c * elemsize);
            }
        }
        break;
        case IM_DT_INT32:
        {
            uint32_t s_buf[4] = {(uint32_t)(color.r * UINT32_MAX), 
                                 (uint32_t)(color.g * UINT32_MAX),
                                 (uint32_t)(color.b * UINT32_MAX),
                                 (uint32_t)(color.a * UINT32_MAX)};
            for (int i = 0; i < total() / c; i++)
            {
                memcpy((uint8_t*)data + i * elemsize * c, s_buf, c * elemsize);
            }
        }
        break;
        case IM_DT_INT64:
        {
            uint64_t s_buf[4] = {(uint64_t)(color.r * UINT64_MAX), 
                                 (uint64_t)(color.g * UINT64_MAX),
                                 (uint64_t)(color.b * UINT64_MAX),
                                 (uint64_t)(color.a * UINT64_MAX)};
            for (int i = 0; i < total() / c; i++)
            {
                memcpy((uint8_t*)data + i * elemsize * c, s_buf, c * elemsize);
            }
        }
        break;
        case IM_DT_FLOAT32:
        {
            float s_buf[4] = {color.r, color.g, color.b, color.a};
            for (int i = 0; i < total() / c; i++)
            {
                memcpy((uint8_t*)data + i * elemsize * c, s_buf, c * elemsize);
            }
        }
        break;
        case IM_DT_FLOAT64:
        {
            double s_buf[4] = {(double)color.r, (double)color.g, (double)color.b, (double)color.a};
            for (int i = 0; i < total() / c; i++)
            {
                memcpy((uint8_t*)data + i * elemsize * c, s_buf, c * elemsize);
            }
        }
        break;
        case IM_DT_FLOAT16:
        // TODO::Dicky
        break;
        default: break;
    }
}

inline void ImMat::get_pixel(int x, int y, ImPixel& color)
{
    assert(dims == 3);
    assert(x >= 0 && x < w);
    assert(y >= 0 && y < h);
    switch (type)
    {
        case IM_DT_INT8:
            if (c > 0) color.r = (float)at<uint8_t>(x, y, 0) / UINT8_MAX;
            if (c > 1) color.g = (float)at<uint8_t>(x, y, 1) / UINT8_MAX;
            if (c > 2) color.b = (float)at<uint8_t>(x, y, 2) / UINT8_MAX;
            if (c > 3) color.a = (float)at<uint8_t>(x, y, 3) / UINT8_MAX;
        break;
        case IM_DT_INT16:
            if (c > 0) color.r = (float)at<uint16_t>(x, y, 0) / UINT16_MAX;
            if (c > 1) color.g = (float)at<uint16_t>(x, y, 1) / UINT16_MAX;
            if (c > 2) color.b = (float)at<uint16_t>(x, y, 2) / UINT16_MAX;
            if (c > 3) color.a = (float)at<uint16_t>(x, y, 3) / UINT16_MAX;
        break;
        case IM_DT_INT32:
            if (c > 0) color.r = (float)at<uint32_t>(x, y, 0) / (float)UINT32_MAX;
            if (c > 1) color.g = (float)at<uint32_t>(x, y, 1) / (float)UINT32_MAX;
            if (c > 2) color.b = (float)at<uint32_t>(x, y, 2) / (float)UINT32_MAX;
            if (c > 3) color.a = (float)at<uint32_t>(x, y, 3) / (float)UINT32_MAX;
        break;
        case IM_DT_INT64:
            if (c > 0) color.r = (float)at<uint64_t>(x, y, 0) / (float)UINT64_MAX;
            if (c > 1) color.g = (float)at<uint64_t>(x, y, 1) / (float)UINT64_MAX;
            if (c > 2) color.b = (float)at<uint64_t>(x, y, 2) / (float)UINT64_MAX;
            if (c > 3) color.a = (float)at<uint64_t>(x, y, 3) / (float)UINT64_MAX;
        break;
        case IM_DT_FLOAT16:
            // TODO::Dicky
        break;
        case IM_DT_FLOAT32:
            if (c > 0) color.r = at<float>(x, y, 0);
            if (c > 1) color.g = at<float>(x, y, 1);
            if (c > 2) color.b = at<float>(x, y, 2);
            if (c > 3) color.a = at<float>(x, y, 3);
        break;
        case IM_DT_FLOAT64:
            if (c > 0) color.r = (float)at<double>(x, y, 0);
            if (c > 1) color.g = (float)at<double>(x, y, 1);
            if (c > 2) color.b = (float)at<double>(x, y, 2);
            if (c > 3) color.a = (float)at<double>(x, y, 3);
        break;
        default: break;
    }
}

inline void ImMat::get_pixel(ImPoint p, ImPixel& color)
{
    get_pixel((int)p.x, (int)p.y, color);
}

inline void ImMat::draw_dot(int x, int y, ImPixel color)
{
    assert(dims == 3);
    //assert(x >= 0 && x < w);
    //assert(y >= 0 && y < h);
    if (x < 0 || x >= w || y < 0 || y >= h)
        return;
    switch (type)
    {
        case IM_DT_INT8:
            if (c > 0) at<uint8_t>(x, y, 0) = color.r * UINT8_MAX;
            if (c > 1) at<uint8_t>(x, y, 1) = color.g * UINT8_MAX;
            if (c > 2) at<uint8_t>(x, y, 2) = color.b * UINT8_MAX;
            if (c > 3) at<uint8_t>(x, y, 3) = color.a * UINT8_MAX;
        break;
        case IM_DT_INT16:
            if (c > 0) at<uint16_t>(x, y, 0) = color.r * UINT16_MAX;
            if (c > 1) at<uint16_t>(x, y, 1) = color.g * UINT16_MAX;
            if (c > 2) at<uint16_t>(x, y, 2) = color.b * UINT16_MAX;
            if (c > 3) at<uint16_t>(x, y, 3) = color.a * UINT16_MAX;
        break;
        case IM_DT_INT32:
            if (c > 0) at<uint32_t>(x, y, 0) = color.r * (float)UINT32_MAX;
            if (c > 1) at<uint32_t>(x, y, 1) = color.g * (float)UINT32_MAX;
            if (c > 2) at<uint32_t>(x, y, 2) = color.b * (float)UINT32_MAX;
            if (c > 3) at<uint32_t>(x, y, 3) = color.a * (float)UINT32_MAX;
        break;
        case IM_DT_INT64:
            if (c > 0) at<uint64_t>(x, y, 0) = color.r * (float)UINT64_MAX;
            if (c > 1) at<uint64_t>(x, y, 1) = color.g * (float)UINT64_MAX;
            if (c > 2) at<uint64_t>(x, y, 2) = color.b * (float)UINT64_MAX;
            if (c > 3) at<uint64_t>(x, y, 3) = color.a * (float)UINT64_MAX;
        break;
        case IM_DT_FLOAT16:
            // TODO::Dicky
        break;
        case IM_DT_FLOAT32:
            if (c > 0) at<float>(x, y, 0) = color.r;
            if (c > 1) at<float>(x, y, 1) = color.g;
            if (c > 2) at<float>(x, y, 2) = color.b;
            if (c > 3) at<float>(x, y, 3) = color.a;
        break;
        case IM_DT_FLOAT64:
            if (c > 0) at<double>(x, y, 0) = (double)color.r;
            if (c > 1) at<double>(x, y, 1) = (double)color.g;
            if (c > 2) at<double>(x, y, 2) = (double)color.b;
            if (c > 3) at<double>(x, y, 3) = (double)color.a;
        break;
        default: break;
    }
}

inline void ImMat::draw_dot(ImPoint p, ImPixel color)
{
    draw_dot((int)p.x, (int)p.y, color);
}

inline void ImMat::alphablend(int x, int y, float alpha, ImPixel color)
{
    switch (type)
    {
        case IM_DT_INT8:
            if (c > 0) at<uint8_t>(x, y, 0) = at<uint8_t>(x, y, 0) * (1 - alpha) + color.r * alpha * UINT8_MAX;
            if (c > 1) at<uint8_t>(x, y, 1) = at<uint8_t>(x, y, 1) * (1 - alpha) + color.g * alpha * UINT8_MAX;
            if (c > 2) at<uint8_t>(x, y, 2) = at<uint8_t>(x, y, 2) * (1 - alpha) + color.b * alpha * UINT8_MAX;
            if (c > 3) at<uint8_t>(x, y, 3) = (uint8_t)(color.a * UINT8_MAX);
        break;
        case IM_DT_INT16:
            if (c > 0) at<uint16_t>(x, y, 0) = at<uint16_t>(x, y, 0) * (1 - alpha) + color.r * alpha * UINT16_MAX;
            if (c > 1) at<uint16_t>(x, y, 1) = at<uint16_t>(x, y, 1) * (1 - alpha) + color.g * alpha * UINT16_MAX;
            if (c > 2) at<uint16_t>(x, y, 2) = at<uint16_t>(x, y, 2) * (1 - alpha) + color.b * alpha * UINT16_MAX;
            if (c > 3) at<uint16_t>(x, y, 3) = (uint16_t)(color.a * UINT16_MAX);
        break;
        case IM_DT_INT32:
            if (c > 0) at<uint32_t>(x, y, 0) = at<uint32_t>(x, y, 0) * (1 - alpha) + color.r * alpha * (float)UINT32_MAX;
            if (c > 1) at<uint32_t>(x, y, 1) = at<uint32_t>(x, y, 1) * (1 - alpha) + color.g * alpha * (float)UINT32_MAX;
            if (c > 2) at<uint32_t>(x, y, 2) = at<uint32_t>(x, y, 2) * (1 - alpha) + color.b * alpha * (float)UINT32_MAX;
            if (c > 3) at<uint32_t>(x, y, 3) = (uint32_t)(color.a * (float)UINT32_MAX);
        break;
        case IM_DT_INT64:
            if (c > 0) at<uint64_t>(x, y, 0) = at<uint64_t>(x, y, 0) * (1 - alpha) + color.r * alpha * (float)UINT64_MAX;
            if (c > 1) at<uint64_t>(x, y, 1) = at<uint64_t>(x, y, 1) * (1 - alpha) + color.g * alpha * (float)UINT64_MAX;
            if (c > 2) at<uint64_t>(x, y, 2) = at<uint64_t>(x, y, 2) * (1 - alpha) + color.b * alpha * (float)UINT64_MAX;
            if (c > 3) at<uint64_t>(x, y, 3) = (uint64_t)(color.a * (float)UINT64_MAX);
        break;
        case IM_DT_FLOAT16:
            // TODO::Dicky
        break;
        case IM_DT_FLOAT32:
            if (c > 0) at<float>(x, y, 0) = at<float>(x, y, 0) * (1 - alpha) + color.r * alpha;
            if (c > 1) at<float>(x, y, 1) = at<float>(x, y, 1) * (1 - alpha) + color.g * alpha;
            if (c > 2) at<float>(x, y, 2) = at<float>(x, y, 2) * (1 - alpha) + color.b * alpha;
            if (c > 3) at<float>(x, y, 3) = color.a;
        break;
        case IM_DT_FLOAT64:
            if (c > 0) at<double>(x, y, 0) = at<double>(x, y, 0) * (1 - alpha) + color.r * alpha;
            if (c > 1) at<double>(x, y, 1) = at<double>(x, y, 1) * (1 - alpha) + color.g * alpha;
            if (c > 2) at<double>(x, y, 2) = at<double>(x, y, 2) * (1 - alpha) + color.b * alpha;
            if (c > 3) at<double>(x, y, 3) = (double)color.a;
        break;
        default: break;
    }
}

inline void ImMat::draw_line(float x1, float y1, float x2, float y2, float t, ImPixel color)
{
    assert(dims == 3);
    assert(x1 >= 0 && x1 < w);
    assert(x2 >= 0 && x2 < w);
    assert(y1 >= 0 && y1 < h);
    assert(y2 >= 0 && y2 < h);

    int _x0 = CLAMP((int)floorf(fminf(x1, x2) - t), 0, w - 1);
    int _x1 = CLAMP((int) ceilf(fmaxf(x1, x2) + t), 0, w - 1);
    int _y0 = CLAMP((int)floorf(fminf(y1, y2) - t), 0, h - 1);
    int _y1 = CLAMP((int) ceilf(fmaxf(y1, y2) + t), 0, h - 1);
    for (int y = _y0; y <= _y1; y++)
    {
        for (int x = _x0; x <= _x1; x++)
        {
            // capsuleSDF
            float pax = (float)x - x1, pay = (float)y - y1, bax = x2 - x1, bay = y2 - y1;
            float _h = CLAMP((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
            float dx = pax - bax * _h, dy = pay - bay * _h;
            float sdf = sqrtf(dx * dx + dy * dy) - t;
            float alpha = CLAMP(0.5f - sdf, 0.f, 1.f);
            alphablend(x, y, alpha, color);
        }
    }
}

inline void ImMat::draw_line(ImPoint p1, ImPoint p2, float t, ImPixel color)
{
    draw_line(p1.x, p1.y, p2.x, p2.y, t, color);
}

inline void ImMat::draw_circle(float x1, float y1, float r, ImPixel color)
{
    // Bresenham circle
    float x = 0, y = r;
	float p = 3 - (2 * r);
	while (x <= y)
	{
        draw_dot(x1 + x, y1 + y, color);
        draw_dot(x1 - x, y1 + y, color);
        draw_dot(x1 + x, y1 - y, color);
        draw_dot(x1 - x, y1 - y, color);
        draw_dot(x1 + y, y1 + x, color);
		draw_dot(x1 + y, y1 - x, color);
		draw_dot(x1 - y, y1 + x, color);
		draw_dot(x1 - y, y1 - x, color);
		x = x + 1;
		if (p < 0)
			p = p + 4 * x + 6;
		else
		{
			p = p + 4 * (x - y) + 10;
			y = y - 1;
        }
    }
}

inline void ImMat::draw_circle(ImPoint p, float r, ImPixel color)
{
    draw_circle(p.x, p.y, r, color);
}

inline void ImMat::draw_circle(float x1, float y1, float r, float t, ImPixel color)
{
    float perimeter = 2 * M_PI * r;
    int num_segments = perimeter / 8 / t;
    const float a_max = (M_PI * 2.0f) * ((float)num_segments - 1.0f) / (float)num_segments;
    const float a_min = 0;
    float x, y, x0, y0, _x, _y;
    num_segments--;
    for (int i = 0; i <= num_segments; i++)
    {
        const float a = a_min + ((float)i / (float)num_segments) * (a_max - a_min);
        x = x1 + cos(a) * r;
        y = y1 + sin(a) * r;
        if (i == 0)
        {
            x0 = _x = x;
            y0 = _y = y;
        }
        else
        {
            draw_line(_x, _y, x, y, t, color);
            _x = x;
            _y = y;
        }
    }
    draw_line(x, y, x0, y0, t, color);
}

inline void ImMat::draw_circle(ImPoint p, float r, float t, ImPixel color)
{
    draw_circle(p.x, p.y, r, t, color);
}

} // namespace ImGui 

#endif /* __IMMAT_H__ */