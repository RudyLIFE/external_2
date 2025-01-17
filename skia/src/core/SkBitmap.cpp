
/*
 * Copyright 2008 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#include "SkBitmap.h"
#include "SkColorPriv.h"
#include "SkDither.h"
#include "SkFlattenable.h"
#include "SkMallocPixelRef.h"
#include "SkMask.h"
#include "SkOrderedReadBuffer.h"
#include "SkOrderedWriteBuffer.h"
#include "SkPixelRef.h"
#include "SkThread.h"
#include "SkUnPreMultiply.h"
#include "SkUtils.h"
#include "SkPackBits.h"
#include <new>

#include "SkListPtr.h"
#include "SkImageEncoder.h"
#include "SkStream.h"
#include <cutils/properties.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

SK_DEFINE_INST_COUNT(SkBitmap::Allocator)

#if 0
static pthread_mutex_t resMutex = PTHREAD_MUTEX_INITIALIZER;
static int gIndex = 0;

static SkListPtr<SkBitmap> gBitmapList;
#endif
static bool checkDumpProp = false;
static bool isEnableDumpBitmap = false;

static bool isPos32Bits(const Sk64& value) {
    return !value.isNeg() && value.is32();
}

struct MipLevel {
    void*       fPixels;
    uint32_t    fRowBytes;
    uint32_t    fWidth, fHeight;
};

struct SkBitmap::MipMap : SkNoncopyable {
    int32_t fRefCnt;
    int     fLevelCount;
//  MipLevel    fLevel[fLevelCount];
//  Pixels[]

    static MipMap* Alloc(int levelCount, size_t pixelSize) {
        if (levelCount < 0) {
            return NULL;
        }
        Sk64 size;
        size.setMul(levelCount + 1, sizeof(MipLevel));
        size.add(sizeof(MipMap));
        size.add(SkToS32(pixelSize));
        if (!isPos32Bits(size)) {
            return NULL;
        }
        MipMap* mm = (MipMap*)sk_malloc_throw(size.get32());
        mm->fRefCnt = 1;
        mm->fLevelCount = levelCount;
        return mm;
    }

    const MipLevel* levels() const { return (const MipLevel*)(this + 1); }
    MipLevel* levels() { return (MipLevel*)(this + 1); }

    const void* pixels() const { return levels() + fLevelCount; }
    void* pixels() { return levels() + fLevelCount; }

    void ref() {
        if (SK_MaxS32 == sk_atomic_inc(&fRefCnt)) {
            sk_throw();
        }
    }
    void unref() {
        SkASSERT(fRefCnt > 0);
        if (sk_atomic_dec(&fRefCnt) == 1) {
            sk_free(this);
        }
    }
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

static void checkDumpBitmapProp()
{
    //if(checkDumpProp) return;
    
    char value[PROPERTY_VALUE_MAX];
    //property_get("skia.enable.store.bitmap", value, "0");
    property_get("debug.skia.dump_enable", value, "0");

    isEnableDumpBitmap = (atoi(value) == 0) ? false : true;

    if(isEnableDumpBitmap && !checkDumpProp)
    {
        checkDumpProp = true;
        SkDebugf("enable store bitmap, pid:%d", getpid());
    }
    else if(!isEnableDumpBitmap && checkDumpProp)
    {
        checkDumpProp = false;
        SkDebugf("disable store bitmap, pid:%d", getpid());
#if 0
        gBitmapList.clear();
#endif
    }
    
}
#if 0
static void dumpBitmap()
{
    char value[PROPERTY_VALUE_MAX];
    //property_get("skia.dump.bitmap.pid", value, "0");
    property_get("debug.skia.dump_pid", value, "0");

    if((atoi(value) != getpid()) && (atoi(value) != 1)) return;

    //property_get("skia.dump.bitmap.path", value, "/data");
    property_get("debug.skia.dump_path", value, "/data");
    char file_path[100];

    SkListPtr<SkBitmap>::iterator it;

    for(it = gBitmapList.begin() ; it != gBitmapList.end() ; it++, gIndex++)
    {
        if(it->width() == 0 || it->height() == 0 || it->getPixels() == NULL) continue;
        sprintf(file_path, "%s/bitmap_%d_%d_%d_%d.jpg", value, getpid(), it->width(), it->height(), gIndex);
        SkDebugf("store bitmap (%p) to %s", &(*it), file_path);
        SkWStream* strm = new SkFILEWStream(file_path);
        SkImageEncoder* encoder = SkImageEncoder::Create(SkImageEncoder::kJPEG_Type);
        if (NULL != encoder) 
        {
            encoder->encodeStream(strm, *it, 70);
            delete encoder;
        }
        delete strm;
    }
    gBitmapList.clear();
    
}
#endif

static void dumpBitmap(SkBitmap* bm)
{
    char value[PROPERTY_VALUE_MAX];
    //property_get("skia.dump.bitmap.pid", value, "0");
    property_get("debug.skia.dump_pid", value, "0");

    if((atoi(value) != getpid()) && (atoi(value) != 1)) return;

    //property_get("skia.dump.bitmap.path", value, "/data");
    property_get("debug.skia.dump_path", value, "/data");
    char file_path[100];

    sprintf(file_path, "%s/bitmap_%d_%d_%d.jpg", value, getpid(), bm->width(), bm->height());
    SkDebugf("store bitmap (%p) to %s", bm, file_path);
    SkWStream* strm = new SkFILEWStream(file_path);
    SkImageEncoder* encoder = SkImageEncoder::Create(SkImageEncoder::kJPEG_Type);
    if (NULL != encoder) 
    {
        encoder->encodeStream(strm, *bm, 70); //thread 1
        delete encoder;
    }
    delete strm;
}


SkBitmap::SkBitmap() {
    sk_bzero(this, sizeof(*this));
#if 0
    checkDumpBitmapProp();
    if(isEnableDumpBitmap) 
    {
        pthread_mutex_lock(&resMutex);
        gBitmapList.push_back(this);
        pthread_mutex_unlock(&resMutex);
    }
#endif
}

SkBitmap::SkBitmap(const SkBitmap& src) {
    SkDEBUGCODE(src.validate();)
    sk_bzero(this, sizeof(*this));
    *this = src;
    SkDEBUGCODE(this->validate();)
#if 0
    checkDumpBitmapProp();
    if(isEnableDumpBitmap) 
    {
        pthread_mutex_lock(&resMutex);
        gBitmapList.push_back(this);
        pthread_mutex_unlock(&resMutex);
    }
#endif
}

SkBitmap::~SkBitmap() {
#if 0
    checkDumpBitmapProp();
    if(isEnableDumpBitmap) 
    {
#if 0
        pthread_mutex_lock(&resMutex);
        dumpBitmap();
        SkListPtr<SkBitmap>::iterator it;
        for(it = gBitmapList.begin() ; it != gBitmapList.end() ; it++)
            if(this == &(*it))
            {
                gBitmapList.remove(it);
                break;
            }
        pthread_mutex_unlock(&resMutex);
#else
        dumpBitmap(this);
#endif
    }
#endif
    SkDEBUGCODE(this->validate();)
    this->freePixels();
}

SkBitmap& SkBitmap::operator=(const SkBitmap& src) {
    if (this != &src) {
        this->freePixels();
        memcpy(this, &src, sizeof(src));

        // inc src reference counts
        SkSafeRef(src.fPixelRef);
        SkSafeRef(src.fMipMap);

        // we reset our locks if we get blown away
        fPixelLockCount = 0;

        /*  The src could be in 3 states
            1. no pixelref, in which case we just copy/ref the pixels/ctable
            2. unlocked pixelref, pixels/ctable should be null
            3. locked pixelref, we should lock the ref again ourselves
        */
        if (NULL == fPixelRef) {
            // leave fPixels as it is
            SkSafeRef(fColorTable); // ref the user's ctable if present
        } else {    // we have a pixelref, so pixels/ctable reflect it
            // ignore the values from the memcpy
            fPixels = NULL;
            fColorTable = NULL;
            // Note that what to for genID is somewhat arbitrary. We have no
            // way to track changes to raw pixels across multiple SkBitmaps.
            // Would benefit from an SkRawPixelRef type created by
            // setPixels.
            // Just leave the memcpy'ed one but they'll get out of sync
            // as soon either is modified.
        }
    }

    SkDEBUGCODE(this->validate();)
    return *this;
}

void SkBitmap::swap(SkBitmap& other) {
    SkTSwap(fColorTable, other.fColorTable);
    SkTSwap(fPixelRef, other.fPixelRef);
    SkTSwap(fPixelRefOffset, other.fPixelRefOffset);
    SkTSwap(fPixelLockCount, other.fPixelLockCount);
    SkTSwap(fMipMap, other.fMipMap);
    SkTSwap(fPixels, other.fPixels);
    SkTSwap(fRowBytes, other.fRowBytes);
    SkTSwap(fWidth, other.fWidth);
    SkTSwap(fHeight, other.fHeight);
    SkTSwap(fConfig, other.fConfig);
    SkTSwap(fFlags, other.fFlags);
    SkTSwap(fBytesPerPixel, other.fBytesPerPixel);

    SkDEBUGCODE(this->validate();)
}

void SkBitmap::reset() {
    this->freePixels();
    sk_bzero(this, sizeof(*this));
}

int SkBitmap::ComputeBytesPerPixel(SkBitmap::Config config) {
    int bpp;
    switch (config) {
        case kNo_Config:
        case kA1_Config:
            bpp = 0;   // not applicable
            break;
        case kA8_Config:
        case kIndex8_Config:
            bpp = 1;
            break;
        case kRGB_565_Config:
        case kARGB_4444_Config:
            bpp = 2;
            break;
        case kARGB_8888_Config:
            bpp = 4;
            break;
        default:
            SkDEBUGFAIL("unknown config");
            bpp = 0;   // error
            break;
    }
    return bpp;
}

size_t SkBitmap::ComputeRowBytes(Config c, int width) {
    if (width < 0) {
        return 0;
    }

    Sk64 rowBytes;
    rowBytes.setZero();

    switch (c) {
        case kNo_Config:
            break;
        case kA1_Config:
            rowBytes.set(width);
            rowBytes.add(7);
            rowBytes.shiftRight(3);
            break;
        case kA8_Config:
        case kIndex8_Config:
            rowBytes.set(width);
            break;
        case kRGB_565_Config:
        case kARGB_4444_Config:
            rowBytes.set(width);
            rowBytes.shiftLeft(1);
            break;
        case kARGB_8888_Config:
            rowBytes.set(width);
            rowBytes.shiftLeft(2);
            break;
        default:
            SkDEBUGFAIL("unknown config");
            break;
    }
    return isPos32Bits(rowBytes) ? rowBytes.get32() : 0;
}

Sk64 SkBitmap::ComputeSize64(Config c, int width, int height) {
    Sk64 size;
    size.setMul(SkToS32(SkBitmap::ComputeRowBytes(c, width)), height);
    return size;
}

size_t SkBitmap::ComputeSize(Config c, int width, int height) {
    Sk64 size = SkBitmap::ComputeSize64(c, width, height);
    return isPos32Bits(size) ? size.get32() : 0;
}

Sk64 SkBitmap::ComputeSafeSize64(Config config,
                                 uint32_t width,
                                 uint32_t height,
                                 size_t rowBytes) {
    Sk64 safeSize;
    safeSize.setZero();
    if (height > 0) {
        // TODO: Handle the case where the return value from
        // ComputeRowBytes is more than 31 bits.
        safeSize.set(SkToS32(ComputeRowBytes(config, width)));
        Sk64 sizeAllButLastRow;
        sizeAllButLastRow.setMul(height - 1, SkToS32(rowBytes));
        safeSize.add(sizeAllButLastRow);
    }
    SkASSERT(!safeSize.isNeg());
    return safeSize;
}

size_t SkBitmap::ComputeSafeSize(Config config,
                                 uint32_t width,
                                 uint32_t height,
                                 size_t rowBytes) {
    Sk64 safeSize = ComputeSafeSize64(config, width, height, rowBytes);
    return (safeSize.is32() ? safeSize.get32() : 0);
}

void SkBitmap::getBounds(SkRect* bounds) const {
    SkASSERT(bounds);
    bounds->set(0, 0,
                SkIntToScalar(fWidth), SkIntToScalar(fHeight));
}

void SkBitmap::getBounds(SkIRect* bounds) const {
    SkASSERT(bounds);
    bounds->set(0, 0, fWidth, fHeight);
}

///////////////////////////////////////////////////////////////////////////////

void SkBitmap::setConfig(Config c, int width, int height, size_t rowBytes) {
    this->freePixels();

    if ((width | height) < 0) {
        goto err;
    }

    if (rowBytes == 0) {
        rowBytes = SkBitmap::ComputeRowBytes(c, width);
        if (0 == rowBytes && kNo_Config != c) {
            goto err;
        }
    }

    fConfig     = SkToU8(c);
    fWidth      = width;
    fHeight     = height;
    fRowBytes   = SkToU32(rowBytes);

    fBytesPerPixel = (uint8_t)ComputeBytesPerPixel(c);

    SkDEBUGCODE(this->validate();)
    return;

    // if we got here, we had an error, so we reset the bitmap to empty
err:
    this->reset();
}

void SkBitmap::updatePixelsFromRef() const {
    if (NULL != fPixelRef) {
        if (fPixelLockCount > 0) {
            SkASSERT(fPixelRef->isLocked());

            void* p = fPixelRef->pixels();
            if (NULL != p) {
                p = (char*)p + fPixelRefOffset;
            }
            fPixels = p;
            SkRefCnt_SafeAssign(fColorTable, fPixelRef->colorTable());
        } else {
            SkASSERT(0 == fPixelLockCount);
            fPixels = NULL;
            if (fColorTable) {
                fColorTable->unref();
                fColorTable = NULL;
            }
        }
    }
}

SkPixelRef* SkBitmap::setPixelRef(SkPixelRef* pr, size_t offset) {
    // do this first, we that we never have a non-zero offset with a null ref
    if (NULL == pr) {
        offset = 0;
    }

    if (fPixelRef != pr || fPixelRefOffset != offset) {
        if (fPixelRef != pr) {
            this->freePixels();
            SkASSERT(NULL == fPixelRef);

            SkSafeRef(pr);
            fPixelRef = pr;
        }
        fPixelRefOffset = offset;
        this->updatePixelsFromRef();
    }

    SkDEBUGCODE(this->validate();)
    return pr;
}

void SkBitmap::lockPixels() const {
    if (NULL != fPixelRef && 0 == sk_atomic_inc(&fPixelLockCount)) {
        fPixelRef->lockPixels();
        this->updatePixelsFromRef();
    }
    SkDEBUGCODE(this->validate();)
}

void SkBitmap::unlockPixels() const {
    SkASSERT(NULL == fPixelRef || fPixelLockCount > 0);

    if (NULL != fPixelRef && 1 == sk_atomic_dec(&fPixelLockCount)) {
        fPixelRef->unlockPixels();
        this->updatePixelsFromRef();
    }
    SkDEBUGCODE(this->validate();)
}

bool SkBitmap::lockPixelsAreWritable() const {
    return (fPixelRef) ? fPixelRef->lockPixelsAreWritable() : false;
}

void SkBitmap::setPixels(void* p, SkColorTable* ctable) {
    if (NULL == p) {
        this->setPixelRef(NULL, 0);
        return;
    }

    Sk64 size = this->getSize64();
    SkASSERT(!size.isNeg() && size.is32());

    this->setPixelRef(new SkMallocPixelRef(p, size.get32(), ctable, false))->unref();
    // since we're already allocated, we lockPixels right away
    this->lockPixels();
    SkDEBUGCODE(this->validate();)
}

bool SkBitmap::allocPixels(Allocator* allocator, SkColorTable* ctable) {
    HeapAllocator stdalloc;

    if (NULL == allocator) {
        allocator = &stdalloc;
    }
    return allocator->allocPixelRef(this, ctable);
}

void SkBitmap::freePixels() {
    // if we're gonna free the pixels, we certainly need to free the mipmap
    this->freeMipMap();

    if (fColorTable) {
        fColorTable->unref();
        fColorTable = NULL;
    }

    if (NULL != fPixelRef) {
        if (fPixelLockCount > 0) {
            fPixelRef->unlockPixels();
        }
        fPixelRef->unref();
        fPixelRef = NULL;
        fPixelRefOffset = 0;
    }
    fPixelLockCount = 0;
    fPixels = NULL;
}

void SkBitmap::freeMipMap() {
    if (fMipMap) {
        fMipMap->unref();
        fMipMap = NULL;
    }
}

uint32_t SkBitmap::getGenerationID() const {
    return (fPixelRef) ? fPixelRef->getGenerationID() : 0;
}

void SkBitmap::notifyPixelsChanged() const {
    SkASSERT(!this->isImmutable());
    if (fPixelRef) {
        fPixelRef->notifyPixelsChanged();
    }
}

GrTexture* SkBitmap::getTexture() const {
    return fPixelRef ? fPixelRef->getTexture() : NULL;
}

///////////////////////////////////////////////////////////////////////////////

/** We explicitly use the same allocator for our pixels that SkMask does,
 so that we can freely assign memory allocated by one class to the other.
 */
bool SkBitmap::HeapAllocator::allocPixelRef(SkBitmap* dst,
                                            SkColorTable* ctable) {
    Sk64 size = dst->getSize64();
    if (size.isNeg() || !size.is32()) {
        return false;
    }

    void* addr = sk_malloc_flags(size.get32(), 0);  // returns NULL on failure
    if (NULL == addr) {
        return false;
    }

    dst->setPixelRef(new SkMallocPixelRef(addr, size.get32(), ctable))->unref();
    // since we're already allocated, we lockPixels right away
    dst->lockPixels();
    return true;
}

///////////////////////////////////////////////////////////////////////////////

size_t SkBitmap::getSafeSize() const {
    // This is intended to be a size_t version of ComputeSafeSize64(), just
    // faster. The computation is meant to be identical.
    return (fHeight ? ((fHeight - 1) * fRowBytes) +
            ComputeRowBytes(getConfig(), fWidth): 0);
}

Sk64 SkBitmap::getSafeSize64() const {
    return ComputeSafeSize64(getConfig(), fWidth, fHeight, fRowBytes);
}

bool SkBitmap::copyPixelsTo(void* const dst, size_t dstSize,
                            size_t dstRowBytes, bool preserveDstPad) const {

    if (0 == dstRowBytes) {
        dstRowBytes = fRowBytes;
    }

    if (dstRowBytes < ComputeRowBytes(getConfig(), fWidth) ||
        dst == NULL || (getPixels() == NULL && pixelRef() == NULL))
        return false;

    if (!preserveDstPad && static_cast<uint32_t>(dstRowBytes) == fRowBytes) {
        size_t safeSize = getSafeSize();
        if (safeSize > dstSize || safeSize == 0)
            return false;
        else {
            SkAutoLockPixels lock(*this);
            // This implementation will write bytes beyond the end of each row,
            // excluding the last row, if the bitmap's stride is greater than
            // strictly required by the current config.
            memcpy(dst, getPixels(), safeSize);

            return true;
        }
    } else {
        // If destination has different stride than us, then copy line by line.
        if (ComputeSafeSize(getConfig(), fWidth, fHeight, dstRowBytes) >
            dstSize)
            return false;
        else {
            // Just copy what we need on each line.
            size_t rowBytes = ComputeRowBytes(getConfig(), fWidth);
            SkAutoLockPixels lock(*this);
            const uint8_t* srcP = reinterpret_cast<const uint8_t*>(getPixels());
            uint8_t* dstP = reinterpret_cast<uint8_t*>(dst);
            for (uint32_t row = 0; row < fHeight;
                 row++, srcP += fRowBytes, dstP += dstRowBytes) {
                memcpy(dstP, srcP, rowBytes);
            }

            return true;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

bool SkBitmap::isImmutable() const {
    return fPixelRef ? fPixelRef->isImmutable() :
        fFlags & kImageIsImmutable_Flag;
}

void SkBitmap::setImmutable() {
    if (fPixelRef) {
        fPixelRef->setImmutable();
    } else {
        fFlags |= kImageIsImmutable_Flag;
    }
}

bool SkBitmap::isOpaque() const {
    switch (fConfig) {
        case kNo_Config:
            return true;

        case kA1_Config:
        case kA8_Config:
        case kARGB_4444_Config:
        case kARGB_8888_Config:
            return (fFlags & kImageIsOpaque_Flag) != 0;

        case kIndex8_Config: {
            uint32_t flags = 0;

            this->lockPixels();
            // if lockPixels failed, we may not have a ctable ptr
            if (fColorTable) {
                flags = fColorTable->getFlags();
            }
            this->unlockPixels();

            return (flags & SkColorTable::kColorsAreOpaque_Flag) != 0;
        }

        case kRGB_565_Config:
            return true;

        default:
            SkDEBUGFAIL("unknown bitmap config pased to isOpaque");
            return false;
    }
}

void SkBitmap::setIsOpaque(bool isOpaque) {
    /*  we record this regardless of fConfig, though it is ignored in
        isOpaque() for configs that can't support per-pixel alpha.
    */
    if (isOpaque) {
        fFlags |= kImageIsOpaque_Flag;
    } else {
        fFlags &= ~kImageIsOpaque_Flag;
    }
}

bool SkBitmap::isVolatile() const {
    return (fFlags & kImageIsVolatile_Flag) != 0;
}

void SkBitmap::setIsVolatile(bool isVolatile) {
    if (isVolatile) {
        fFlags |= kImageIsVolatile_Flag;
    } else {
        fFlags &= ~kImageIsVolatile_Flag;
    }
}

void* SkBitmap::getAddr(int x, int y) const {
    SkASSERT((unsigned)x < (unsigned)this->width());
    SkASSERT((unsigned)y < (unsigned)this->height());

    char* base = (char*)this->getPixels();
    if (base) {
        base += y * this->rowBytes();
        switch (this->config()) {
            case SkBitmap::kARGB_8888_Config:
                base += x << 2;
                break;
            case SkBitmap::kARGB_4444_Config:
            case SkBitmap::kRGB_565_Config:
                base += x << 1;
                break;
            case SkBitmap::kA8_Config:
            case SkBitmap::kIndex8_Config:
                base += x;
                break;
            case SkBitmap::kA1_Config:
                base += x >> 3;
                break;
            default:
                SkDEBUGFAIL("Can't return addr for config");
                base = NULL;
                break;
        }
    }
    return base;
}

SkColor SkBitmap::getColor(int x, int y) const {
    SkASSERT((unsigned)x < (unsigned)this->width());
    SkASSERT((unsigned)y < (unsigned)this->height());

    switch (this->config()) {
        case SkBitmap::kA1_Config: {
            uint8_t* addr = this->getAddr1(x, y);
            uint8_t mask = 1 << (7  - (x % 8));
            if (addr[0] & mask) {
                return SK_ColorBLACK;
            } else {
                return 0;
            }
        }
        case SkBitmap::kA8_Config: {
            uint8_t* addr = this->getAddr8(x, y);
            return SkColorSetA(0, addr[0]);
        }
        case SkBitmap::kIndex8_Config: {
            SkPMColor c = this->getIndex8Color(x, y);
            return SkUnPreMultiply::PMColorToColor(c);
        }
        case SkBitmap::kRGB_565_Config: {
            uint16_t* addr = this->getAddr16(x, y);
            return SkPixel16ToColor(addr[0]);
        }
        case SkBitmap::kARGB_4444_Config: {
            uint16_t* addr = this->getAddr16(x, y);
            SkPMColor c = SkPixel4444ToPixel32(addr[0]);
            return SkUnPreMultiply::PMColorToColor(c);
        }
        case SkBitmap::kARGB_8888_Config: {
            uint32_t* addr = this->getAddr32(x, y);
            return SkUnPreMultiply::PMColorToColor(addr[0]);
        }
        case kNo_Config:
            SkASSERT(false);
            return 0;
    }
    SkASSERT(false);  // Not reached.
    return 0;
}

bool SkBitmap::ComputeIsOpaque(const SkBitmap& bm) {
    SkAutoLockPixels alp(bm);
    if (!bm.getPixels()) {
        return false;
    }

    const int height = bm.height();
    const int width = bm.width();

    switch (bm.config()) {
        case SkBitmap::kA1_Config: {
            // TODO
        } break;
        case SkBitmap::kA8_Config: {
            unsigned a = 0xFF;
            for (int y = 0; y < height; ++y) {
                const uint8_t* row = bm.getAddr8(0, y);
                for (int x = 0; x < width; ++x) {
                    a &= row[x];
                }
                if (0xFF != a) {
                    return false;
                }
            }
            return true;
        } break;
        case SkBitmap::kIndex8_Config: {
            SkAutoLockColors alc(bm);
            const SkPMColor* table = alc.colors();
            if (!table) {
                return false;
            }
            SkPMColor c = (SkPMColor)~0;
            for (int i = bm.getColorTable()->count() - 1; i >= 0; --i) {
                c &= table[i];
            }
            return 0xFF == SkGetPackedA32(c);
        } break;
        case SkBitmap::kRGB_565_Config:
            return true;
            break;
        case SkBitmap::kARGB_4444_Config: {
            unsigned c = 0xFFFF;
            for (int y = 0; y < height; ++y) {
                const SkPMColor16* row = bm.getAddr16(0, y);
                for (int x = 0; x < width; ++x) {
                    c &= row[x];
                }
                if (0xF != SkGetPackedA4444(c)) {
                    return false;
                }
            }
            return true;
        } break;
        case SkBitmap::kARGB_8888_Config: {
            SkPMColor c = (SkPMColor)~0;
            for (int y = 0; y < height; ++y) {
                const SkPMColor* row = bm.getAddr32(0, y);
                for (int x = 0; x < width; ++x) {
                    c &= row[x];
                }
                if (0xFF != SkGetPackedA32(c)) {
                    return false;
                }
            }
            return true;
        }
        default:
            break;
    }
    return false;
}


///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

static uint16_t pack_8888_to_4444(unsigned a, unsigned r, unsigned g, unsigned b) {
    unsigned pixel = (SkA32To4444(a) << SK_A4444_SHIFT) |
                     (SkR32To4444(r) << SK_R4444_SHIFT) |
                     (SkG32To4444(g) << SK_G4444_SHIFT) |
                     (SkB32To4444(b) << SK_B4444_SHIFT);
    return SkToU16(pixel);
}

void SkBitmap::internalErase(const SkIRect& area,
                             U8CPU a, U8CPU r, U8CPU g, U8CPU b) const {
#ifdef SK_DEBUG
    SkDEBUGCODE(this->validate();)
    SkASSERT(!area.isEmpty());
    {
        SkIRect total = { 0, 0, this->width(), this->height() };
        SkASSERT(total.contains(area));
    }
#endif

    if (kNo_Config == fConfig || kIndex8_Config == fConfig) {
        return;
    }

    SkAutoLockPixels alp(*this);
    // perform this check after the lock call
    if (!this->readyToDraw()) {
        return;
    }

    int height = area.height();
    const int width = area.width();
    const int rowBytes = fRowBytes;

    // make rgb premultiplied
    if (255 != a) {
        r = SkAlphaMul(r, a);
        g = SkAlphaMul(g, a);
        b = SkAlphaMul(b, a);
    }

    switch (fConfig) {
        case kA1_Config: {
            uint8_t* p = this->getAddr1(area.fLeft, area.fTop);
            const int left = area.fLeft >> 3;
            const int right = area.fRight >> 3;

            int middle = right - left - 1;

            uint8_t leftMask = 0xFF >> (area.fLeft & 7);
            uint8_t rightMask = ~(0xFF >> (area.fRight & 7));
            if (left == right) {
                leftMask &= rightMask;
                rightMask = 0;
            }

            a = (a >> 7) ? 0xFF : 0;
            while (--height >= 0) {
                uint8_t* startP = p;

                *p = (*p & ~leftMask) | (a & leftMask);
                p++;
                if (middle > 0) {
                    memset(p, a, middle);
                    p += middle;
                }
                if (rightMask) {
                    *p = (*p & ~rightMask) | (a & rightMask);
                }

                p = startP + rowBytes;
            }
            break;
        }
        case kA8_Config: {
            uint8_t* p = this->getAddr8(area.fLeft, area.fTop);
            while (--height >= 0) {
                memset(p, a, width);
                p += rowBytes;
            }
            break;
        }
        case kARGB_4444_Config:
        case kRGB_565_Config: {
            uint16_t* p = this->getAddr16(area.fLeft, area.fTop);;
            uint16_t v;

            if (kARGB_4444_Config == fConfig) {
                v = pack_8888_to_4444(a, r, g, b);
            } else {
                v = SkPackRGB16(r >> (8 - SK_R16_BITS),
                                g >> (8 - SK_G16_BITS),
                                b >> (8 - SK_B16_BITS));
            }
            while (--height >= 0) {
                sk_memset16(p, v, width);
                p = (uint16_t*)((char*)p + rowBytes);
            }
            break;
        }
        case kARGB_8888_Config: {
            uint32_t* p = this->getAddr32(area.fLeft, area.fTop);
            uint32_t  v = SkPackARGB32(a, r, g, b);

            while (--height >= 0) {
                sk_memset32(p, v, width);
                p = (uint32_t*)((char*)p + rowBytes);
            }
            break;
        }
    }

    this->notifyPixelsChanged();
}

void SkBitmap::eraseARGB(U8CPU a, U8CPU r, U8CPU g, U8CPU b) const {
    SkIRect area = { 0, 0, this->width(), this->height() };
    if (!area.isEmpty()) {
        this->internalErase(area, a, r, g, b);
    }
}

void SkBitmap::eraseArea(const SkIRect& rect, SkColor c) const {
    SkIRect area = { 0, 0, this->width(), this->height() };
    if (area.intersect(rect)) {
        this->internalErase(area, SkColorGetA(c), SkColorGetR(c),
                            SkColorGetG(c), SkColorGetB(c));
    }
}

//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////

#define SUB_OFFSET_FAILURE  ((size_t)-1)

/**
 *  Based on the Config and rowBytes() of bm, return the offset into an SkPixelRef of the pixel at
 *  (x, y).
 *  Note that the SkPixelRef does not need to be set yet. deepCopyTo takes advantage of this fact.
 *  Also note that (x, y) may be outside the range of (0 - width(), 0 - height()), so long as it is
 *  within the bounds of the SkPixelRef being used.
 */
static size_t get_sub_offset(const SkBitmap& bm, int x, int y) {
    switch (bm.getConfig()) {
        case SkBitmap::kA8_Config:
        case SkBitmap:: kIndex8_Config:
            // x is fine as is for the calculation
            break;

        case SkBitmap::kRGB_565_Config:
        case SkBitmap::kARGB_4444_Config:
            x <<= 1;
            break;

        case SkBitmap::kARGB_8888_Config:
            x <<= 2;
            break;

        case SkBitmap::kNo_Config:
        case SkBitmap::kA1_Config:
        default:
            return SUB_OFFSET_FAILURE;
    }
    return y * bm.rowBytes() + x;
}

/**
 *  Using the pixelRefOffset(), rowBytes(), and Config of bm, determine the (x, y) coordinate of the
 *  upper left corner of bm relative to its SkPixelRef.
 *  x and y must be non-NULL.
 */
bool get_upper_left_from_offset(SkBitmap::Config config, size_t offset, size_t rowBytes,
                                   int32_t* x, int32_t* y);
bool get_upper_left_from_offset(SkBitmap::Config config, size_t offset, size_t rowBytes,
                                   int32_t* x, int32_t* y) {
    SkASSERT(x != NULL && y != NULL);
    if (0 == offset) {
        *x = *y = 0;
        return true;
    }
    // Use integer division to find the correct y position.
    *y = SkToS32(offset / rowBytes);
    // The remainder will be the x position, after we reverse get_sub_offset.
    *x = SkToS32(offset % rowBytes);
    switch (config) {
        case SkBitmap::kA8_Config:
            // Fall through.
        case SkBitmap::kIndex8_Config:
            // x is unmodified
            break;

        case SkBitmap::kRGB_565_Config:
            // Fall through.
        case SkBitmap::kARGB_4444_Config:
            *x >>= 1;
            break;

        case SkBitmap::kARGB_8888_Config:
            *x >>= 2;
            break;

        case SkBitmap::kNo_Config:
            // Fall through.
        case SkBitmap::kA1_Config:
            // Fall through.
        default:
            return false;
    }
    return true;
}

static bool get_upper_left_from_offset(const SkBitmap& bm, int32_t* x, int32_t* y) {
    return get_upper_left_from_offset(bm.config(), bm.pixelRefOffset(), bm.rowBytes(), x, y);
}

bool SkBitmap::extractSubset(SkBitmap* result, const SkIRect& subset) const {
    SkDEBUGCODE(this->validate();)

    if (NULL == result || NULL == fPixelRef) {
        return false;   // no src pixels
    }

    SkIRect srcRect, r;
    srcRect.set(0, 0, this->width(), this->height());
    if (!r.intersect(srcRect, subset)) {
        return false;   // r is empty (i.e. no intersection)
    }

    if (fPixelRef->getTexture() != NULL) {
        // Do a deep copy
        SkPixelRef* pixelRef = fPixelRef->deepCopy(this->config(), &subset);
        if (pixelRef != NULL) {
            SkBitmap dst;
            dst.setConfig(this->config(), subset.width(), subset.height());
            dst.setIsVolatile(this->isVolatile());
            dst.setIsOpaque(this->isOpaque());
            dst.setPixelRef(pixelRef)->unref();
            SkDEBUGCODE(dst.validate());
            result->swap(dst);
            return true;
        }
    }

    // If the upper left of the rectangle was outside the bounds of this SkBitmap, we should have
    // exited above.
    SkASSERT(static_cast<unsigned>(r.fLeft) < static_cast<unsigned>(this->width()));
    SkASSERT(static_cast<unsigned>(r.fTop) < static_cast<unsigned>(this->height()));

    size_t offset = get_sub_offset(*this, r.fLeft, r.fTop);
    if (SUB_OFFSET_FAILURE == offset) {
        return false;   // config not supported
    }

    SkBitmap dst;
    dst.setConfig(this->config(), r.width(), r.height(), this->rowBytes());
    dst.setIsVolatile(this->isVolatile());
    // we need to exclude kIndex8_Config for avoid race condition in updatePixelsFromRef
    // when unlockPixels in check conditon "this->isOpaque()"
    // for example: a and b thread excute this->isOpaque() at the same time
    // a and b will excute lockpixel unlockpixel with crossing sequence
    if (kIndex8_Config != fConfig) {
        dst.setIsOpaque(this->isOpaque());
    }

    if (fPixelRef) {
        // share the pixelref with a custom offset
        dst.setPixelRef(fPixelRef, fPixelRefOffset + offset);
    }
    SkDEBUGCODE(dst.validate();)

    // we know we're good, so commit to result
    result->swap(dst);
    return true;
}

///////////////////////////////////////////////////////////////////////////////

#include "SkCanvas.h"
#include "SkPaint.h"

bool SkBitmap::canCopyTo(Config dstConfig) const {
    if (this->getConfig() == kNo_Config) {
        return false;
    }

    bool sameConfigs = (this->config() == dstConfig);
    switch (dstConfig) {
        case kA8_Config:
        case kRGB_565_Config:
        case kARGB_8888_Config:
            break;
        case kA1_Config:
        case kIndex8_Config:
            if (!sameConfigs) {
                return false;
            }
            break;
        case kARGB_4444_Config:
            return sameConfigs || kARGB_8888_Config == this->config();
        default:
            return false;
    }

    // do not copy src if srcConfig == kA1_Config while dstConfig != kA1_Config
    if (this->getConfig() == kA1_Config && !sameConfigs) {
        return false;
    }

    return true;
}

bool SkBitmap::copyTo(SkBitmap* dst, Config dstConfig, Allocator* alloc) const {
    if (!this->canCopyTo(dstConfig)) {
        return false;
    }

    // if we have a texture, first get those pixels
    SkBitmap tmpSrc;
    const SkBitmap* src = this;

    if (fPixelRef) {
        SkIRect subset;
        if (get_upper_left_from_offset(*this, &subset.fLeft, &subset.fTop)) {
            subset.fRight = subset.fLeft + fWidth;
            subset.fBottom = subset.fTop + fHeight;
            if (fPixelRef->readPixels(&tmpSrc, &subset)) {
                SkASSERT(tmpSrc.width() == this->width());
                SkASSERT(tmpSrc.height() == this->height());

                // did we get lucky and we can just return tmpSrc?
                if (tmpSrc.config() == dstConfig && NULL == alloc) {
                    dst->swap(tmpSrc);
                    if (dst->pixelRef() && this->config() == dstConfig) {
                        dst->pixelRef()->fGenerationID = fPixelRef->getGenerationID();
                    }
                    return true;
                }

                // fall through to the raster case
                src = &tmpSrc;
            }
        }
    }

    // we lock this now, since we may need its colortable
    SkAutoLockPixels srclock(*src);
    if (!src->readyToDraw()) {
        return false;
    }

    SkBitmap tmpDst;
    tmpDst.setConfig(dstConfig, src->width(), src->height());

    // allocate colortable if srcConfig == kIndex8_Config
    SkColorTable* ctable = (dstConfig == kIndex8_Config) ?
    new SkColorTable(*src->getColorTable()) : NULL;
    SkAutoUnref au(ctable);
    if (!tmpDst.allocPixels(alloc, ctable)) {
        return false;
    }

    if (!tmpDst.readyToDraw()) {
        // allocator/lock failed
        return false;
    }

    /* do memcpy for the same configs cases, else use drawing
    */
    if (src->config() == dstConfig) {
        if (tmpDst.getSize() == src->getSize()) {
            memcpy(tmpDst.getPixels(), src->getPixels(), src->getSafeSize());
            SkPixelRef* pixelRef = tmpDst.pixelRef();
            if (pixelRef != NULL) {
                pixelRef->fGenerationID = this->getGenerationID();
            }
        } else {
            const char* srcP = reinterpret_cast<const char*>(src->getPixels());
            char* dstP = reinterpret_cast<char*>(tmpDst.getPixels());
            // to be sure we don't read too much, only copy our logical pixels
            size_t bytesToCopy = tmpDst.width() * tmpDst.bytesPerPixel();
            for (int y = 0; y < tmpDst.height(); y++) {
                memcpy(dstP, srcP, bytesToCopy);
                srcP += src->rowBytes();
                dstP += tmpDst.rowBytes();
            }
        }
    } else if (SkBitmap::kARGB_4444_Config == dstConfig
               && SkBitmap::kARGB_8888_Config == src->config()) {
        SkASSERT(src->height() == tmpDst.height());
        SkASSERT(src->width() == tmpDst.width());
        for (int y = 0; y < src->height(); ++y) {
            SkPMColor16* SK_RESTRICT dstRow = (SkPMColor16*) tmpDst.getAddr16(0, y);
            SkPMColor* SK_RESTRICT srcRow = (SkPMColor*) src->getAddr32(0, y);
            DITHER_4444_SCAN(y);
            for (int x = 0; x < src->width(); ++x) {
                dstRow[x] = SkDitherARGB32To4444(srcRow[x],
                                                 DITHER_VALUE(x));
            }
        }
    } else {
        // if the src has alpha, we have to clear the dst first
        if (!src->isOpaque()) {
            tmpDst.eraseColor(SK_ColorTRANSPARENT);
        }

        SkCanvas canvas(tmpDst);
        SkPaint  paint;

        paint.setDither(true);
        canvas.drawBitmap(*src, 0, 0, &paint);
    }

    tmpDst.setIsOpaque(src->isOpaque());

    dst->swap(tmpDst);
    return true;
}

bool SkBitmap::deepCopyTo(SkBitmap* dst, Config dstConfig) const {
    if (!this->canCopyTo(dstConfig)) {
        return false;
    }

    // If we have a PixelRef, and it supports deep copy, use it.
    // Currently supported only by texture-backed bitmaps.
    if (fPixelRef) {
        SkPixelRef* pixelRef = fPixelRef->deepCopy(dstConfig);
        if (pixelRef) {
            uint32_t rowBytes;
            if (dstConfig == fConfig) {
                pixelRef->fGenerationID = fPixelRef->getGenerationID();
                // Use the same rowBytes as the original.
                rowBytes = fRowBytes;
            } else {
                // With the new config, an appropriate fRowBytes will be computed by setConfig.
                rowBytes = 0;
            }
            dst->setConfig(dstConfig, fWidth, fHeight, rowBytes);

            size_t pixelRefOffset;
            if (0 == fPixelRefOffset || dstConfig == fConfig) {
                // Use the same offset as the original.
                pixelRefOffset = fPixelRefOffset;
            } else {
                // Find the correct offset in the new config. This needs to be done after calling
                // setConfig so dst's fConfig and fRowBytes have been set properly.
                int32_t x, y;
                if (!get_upper_left_from_offset(*this, &x, &y)) {
                    return false;
                }
                pixelRefOffset = get_sub_offset(*dst, x, y);
                if (SUB_OFFSET_FAILURE == pixelRefOffset) {
                    return false;
                }
            }
            dst->setPixelRef(pixelRef, pixelRefOffset)->unref();
            return true;
        }
    }

    if (this->getTexture()) {
        return false;
    } else {
        return this->copyTo(dst, dstConfig, NULL);
    }
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

static void downsampleby2_proc32(SkBitmap* dst, int x, int y,
                                 const SkBitmap& src) {
    x <<= 1;
    y <<= 1;
    const SkPMColor* p = src.getAddr32(x, y);
    const SkPMColor* baseP = p;
    SkPMColor c, ag, rb;

    c = *p; ag = (c >> 8) & 0xFF00FF; rb = c & 0xFF00FF;
    if (x < src.width() - 1) {
        p += 1;
    }
    c = *p; ag += (c >> 8) & 0xFF00FF; rb += c & 0xFF00FF;

    p = baseP;
    if (y < src.height() - 1) {
        p += src.rowBytes() >> 2;
    }
    c = *p; ag += (c >> 8) & 0xFF00FF; rb += c & 0xFF00FF;
    if (x < src.width() - 1) {
        p += 1;
    }
    c = *p; ag += (c >> 8) & 0xFF00FF; rb += c & 0xFF00FF;

    *dst->getAddr32(x >> 1, y >> 1) =
        ((rb >> 2) & 0xFF00FF) | ((ag << 6) & 0xFF00FF00);
}

static inline uint32_t expand16(U16CPU c) {
    return (c & ~SK_G16_MASK_IN_PLACE) | ((c & SK_G16_MASK_IN_PLACE) << 16);
}

// returns dirt in the top 16bits, but we don't care, since we only
// store the low 16bits.
static inline U16CPU pack16(uint32_t c) {
    return (c & ~SK_G16_MASK_IN_PLACE) | ((c >> 16) & SK_G16_MASK_IN_PLACE);
}

static void downsampleby2_proc16(SkBitmap* dst, int x, int y,
                                 const SkBitmap& src) {
    x <<= 1;
    y <<= 1;
    const uint16_t* p = src.getAddr16(x, y);
    const uint16_t* baseP = p;
    SkPMColor       c;

    c = expand16(*p);
    if (x < src.width() - 1) {
        p += 1;
    }
    c += expand16(*p);

    p = baseP;
    if (y < src.height() - 1) {
        p += src.rowBytes() >> 1;
    }
    c += expand16(*p);
    if (x < src.width() - 1) {
        p += 1;
    }
    c += expand16(*p);

    *dst->getAddr16(x >> 1, y >> 1) = (uint16_t)pack16(c >> 2);
}

static uint32_t expand4444(U16CPU c) {
    return (c & 0xF0F) | ((c & ~0xF0F) << 12);
}

static U16CPU collaps4444(uint32_t c) {
    return (c & 0xF0F) | ((c >> 12) & ~0xF0F);
}

static void downsampleby2_proc4444(SkBitmap* dst, int x, int y,
                                   const SkBitmap& src) {
    x <<= 1;
    y <<= 1;
    const uint16_t* p = src.getAddr16(x, y);
    const uint16_t* baseP = p;
    uint32_t        c;

    c = expand4444(*p);
    if (x < src.width() - 1) {
        p += 1;
    }
    c += expand4444(*p);

    p = baseP;
    if (y < src.height() - 1) {
        p += src.rowBytes() >> 1;
    }
    c += expand4444(*p);
    if (x < src.width() - 1) {
        p += 1;
    }
    c += expand4444(*p);

    *dst->getAddr16(x >> 1, y >> 1) = (uint16_t)collaps4444(c >> 2);
}

void SkBitmap::buildMipMap(bool forceRebuild) {
    if (forceRebuild)
        this->freeMipMap();
    else if (fMipMap)
        return; // we're already built

    SkASSERT(NULL == fMipMap);

    void (*proc)(SkBitmap* dst, int x, int y, const SkBitmap& src);

    const SkBitmap::Config config = this->getConfig();

    switch (config) {
        case kARGB_8888_Config:
            proc = downsampleby2_proc32;
            break;
        case kRGB_565_Config:
            proc = downsampleby2_proc16;
            break;
        case kARGB_4444_Config:
            proc = downsampleby2_proc4444;
            break;
        case kIndex8_Config:
        case kA8_Config:
        default:
            return; // don't build mipmaps for these configs
    }

    SkAutoLockPixels alp(*this);
    if (!this->readyToDraw()) {
        return;
    }

    // whip through our loop to compute the exact size needed
    size_t  size = 0;
    int     maxLevels = 0;
    {
        int width = this->width();
        int height = this->height();
        for (;;) {
            width >>= 1;
            height >>= 1;
            if (0 == width || 0 == height) {
                break;
            }
            size += ComputeRowBytes(config, width) * height;
            maxLevels += 1;
        }
    }

    // nothing to build
    if (0 == maxLevels) {
        return;
    }

    SkBitmap srcBM(*this);
    srcBM.lockPixels();
    if (!srcBM.readyToDraw()) {
        return;
    }

    MipMap* mm = MipMap::Alloc(maxLevels, size);
    if (NULL == mm) {
        return;
    }

    MipLevel*   level = mm->levels();
    uint8_t*    addr = (uint8_t*)mm->pixels();
    int         width = this->width();
    int         height = this->height();
    uint32_t    rowBytes;
    SkBitmap    dstBM;

    for (int i = 0; i < maxLevels; i++) {
        width >>= 1;
        height >>= 1;
        rowBytes = SkToU32(ComputeRowBytes(config, width));

        level[i].fPixels   = addr;
        level[i].fWidth    = width;
        level[i].fHeight   = height;
        level[i].fRowBytes = rowBytes;

        dstBM.setConfig(config, width, height, rowBytes);
        dstBM.setPixels(addr);

        srcBM.lockPixels();
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                proc(&dstBM, x, y, srcBM);
            }
        }
        srcBM.unlockPixels();

        srcBM = dstBM;
        addr += height * rowBytes;
    }
    SkASSERT(addr == (uint8_t*)mm->pixels() + size);
    fMipMap = mm;
}

bool SkBitmap::hasMipMap() const {
    return fMipMap != NULL;
}

int SkBitmap::extractMipLevel(SkBitmap* dst, SkFixed sx, SkFixed sy) {
    if (NULL == fMipMap) {
        return 0;
    }

    int level = ComputeMipLevel(sx, sy) >> 16;
    SkASSERT(level >= 0);
    if (level <= 0) {
        return 0;
    }

    if (level >= fMipMap->fLevelCount) {
        level = fMipMap->fLevelCount - 1;
    }
    if (dst) {
        const MipLevel& mip = fMipMap->levels()[level - 1];
        dst->setConfig((SkBitmap::Config)this->config(),
                       mip.fWidth, mip.fHeight, mip.fRowBytes);
        dst->setPixels(mip.fPixels);
    }
    return level;
}

SkFixed SkBitmap::ComputeMipLevel(SkFixed sx, SkFixed sy) {
    sx = SkAbs32(sx);
    sy = SkAbs32(sy);
    if (sx < sy) {
        sx = sy;
    }
    if (sx < SK_Fixed1) {
        return 0;
    }
    int clz = SkCLZ(sx);
    SkASSERT(clz >= 1 && clz <= 15);
    return SkIntToFixed(15 - clz) + ((unsigned)(sx << (clz + 1)) >> 16);
}

///////////////////////////////////////////////////////////////////////////////

static bool GetBitmapAlpha(const SkBitmap& src, uint8_t* SK_RESTRICT alpha,
                           int alphaRowBytes) {
    SkASSERT(alpha != NULL);
    SkASSERT(alphaRowBytes >= src.width());

    SkBitmap::Config config = src.getConfig();
    int              w = src.width();
    int              h = src.height();
    size_t           rb = src.rowBytes();

    SkAutoLockPixels alp(src);
    if (!src.readyToDraw()) {
        // zero out the alpha buffer and return
        while (--h >= 0) {
            memset(alpha, 0, w);
            alpha += alphaRowBytes;
        }
        return false;
    }

    if (SkBitmap::kA8_Config == config && !src.isOpaque()) {
        const uint8_t* s = src.getAddr8(0, 0);
        while (--h >= 0) {
            memcpy(alpha, s, w);
            s += rb;
            alpha += alphaRowBytes;
        }
    } else if (SkBitmap::kARGB_8888_Config == config && !src.isOpaque()) {
        const SkPMColor* SK_RESTRICT s = src.getAddr32(0, 0);
        while (--h >= 0) {
#if defined(__ARM_HAVE_NEON) && defined(SK_CPU_LENDIAN)
            asm volatile(
                    "pld        [%[s], #0]                  \n\t"
                    "mov        r6, %[s]                    \n\t"
                    "mov        r7, %[alpha]                \n\t"
                    "mov        r4, %[w]                    \n\t"
                    "subs       r4, r4, #8                  \n\t"
                    "blt        2f                          \n\t"

                    "1:                                     \n\t"
                    "vld4.8     {d0, d1, d2, d3}, [r6]!     \n\t"
                    "subs       r4, r4, #8                  \n\t"
                    "pld        [r6, #64]                   \n\t"
                    "vst1.8     {d3}, [r7]!                 \n\t"
                    "bge        1b                          \n\t"

                    "2:                                     \n\t"
                    "add        r4, r4, #8                  \n\t"
                    "cmp        r4, #0                      \n\t"
                    "ble        4f                          \n\t"

                    "3:                                     \n\t"
                    "ldr        r5, [r6], #4                \n\t"
                    "subs       r4, r4, #1                  \n\t"
                    "lsr        r5, r5, #24                 \n\t"
                    "strb       r5, [r7], #1                \n\t"
                    "bgt        3b                          \n\t"

                    "4:                                     \n\t"
                    :
                    :[w] "r" (w), [s] "r" (s), [alpha] "r" (alpha)
                    :"memory", "r4", "r5", "r6", "r7", "d0", "d1", "d2", "d3"
                    );
#else
            for (int x = 0; x < w; x++) {
                alpha[x] = SkGetPackedA32(s[x]);
            }
#endif
            s = (const SkPMColor*)((const char*)s + rb);
            alpha += alphaRowBytes;
        }
    } else if (SkBitmap::kARGB_4444_Config == config && !src.isOpaque()) {
        const SkPMColor16* SK_RESTRICT s = src.getAddr16(0, 0);
        while (--h >= 0) {
            for (int x = 0; x < w; x++) {
                alpha[x] = SkPacked4444ToA32(s[x]);
            }
            s = (const SkPMColor16*)((const char*)s + rb);
            alpha += alphaRowBytes;
        }
    } else if (SkBitmap::kIndex8_Config == config && !src.isOpaque()) {
        SkColorTable* ct = src.getColorTable();
        if (ct) {
            const SkPMColor* SK_RESTRICT table = ct->lockColors();
            const uint8_t* SK_RESTRICT s = src.getAddr8(0, 0);
            while (--h >= 0) {
                for (int x = 0; x < w; x++) {
                    alpha[x] = SkGetPackedA32(table[s[x]]);
                }
                s += rb;
                alpha += alphaRowBytes;
            }
            ct->unlockColors(false);
        }
    } else {    // src is opaque, so just fill alpha[] with 0xFF
        memset(alpha, 0xFF, h * alphaRowBytes);
    }
    return true;
}

#include "SkPaint.h"
#include "SkMaskFilter.h"
#include "SkMatrix.h"

bool SkBitmap::extractAlpha(SkBitmap* dst, const SkPaint* paint,
                            Allocator *allocator, SkIPoint* offset) const {
    SkDEBUGCODE(this->validate();)

    SkBitmap    tmpBitmap;
    SkMatrix    identity;
    SkMask      srcM, dstM;

    srcM.fBounds.set(0, 0, this->width(), this->height());
    srcM.fRowBytes = SkAlign4(this->width());
    srcM.fFormat = SkMask::kA8_Format;

    SkMaskFilter* filter = paint ? paint->getMaskFilter() : NULL;

    // compute our (larger?) dst bounds if we have a filter
    if (NULL != filter) {
        identity.reset();
        srcM.fImage = NULL;
        if (!filter->filterMask(&dstM, srcM, identity, NULL)) {
            goto NO_FILTER_CASE;
        }
        dstM.fRowBytes = SkAlign4(dstM.fBounds.width());
    } else {
    NO_FILTER_CASE:
        tmpBitmap.setConfig(SkBitmap::kA8_Config, this->width(), this->height(),
                       srcM.fRowBytes);
        if (!tmpBitmap.allocPixels(allocator, NULL)) {
            // Allocation of pixels for alpha bitmap failed.
            SkDebugf("extractAlpha failed to allocate (%d,%d) alpha bitmap\n",
                    tmpBitmap.width(), tmpBitmap.height());
            return false;
        }
        GetBitmapAlpha(*this, tmpBitmap.getAddr8(0, 0), srcM.fRowBytes);
        if (offset) {
            offset->set(0, 0);
        }
        tmpBitmap.swap(*dst);
        return true;
    }
    srcM.fImage = SkMask::AllocImage(srcM.computeImageSize());
    SkAutoMaskFreeImage srcCleanup(srcM.fImage);

    GetBitmapAlpha(*this, srcM.fImage, srcM.fRowBytes);
    if (!filter->filterMask(&dstM, srcM, identity, NULL)) {
        goto NO_FILTER_CASE;
    }
    SkAutoMaskFreeImage dstCleanup(dstM.fImage);

    tmpBitmap.setConfig(SkBitmap::kA8_Config, dstM.fBounds.width(),
                   dstM.fBounds.height(), dstM.fRowBytes);
    if (!tmpBitmap.allocPixels(allocator, NULL)) {
        // Allocation of pixels for alpha bitmap failed.
        SkDebugf("extractAlpha failed to allocate (%d,%d) alpha bitmap\n",
                tmpBitmap.width(), tmpBitmap.height());
        return false;
    }
    memcpy(tmpBitmap.getPixels(), dstM.fImage, dstM.computeImageSize());
    if (offset) {
        offset->set(dstM.fBounds.fLeft, dstM.fBounds.fTop);
    }
    SkDEBUGCODE(tmpBitmap.validate();)

    tmpBitmap.swap(*dst);
    return true;
}

///////////////////////////////////////////////////////////////////////////////

enum {
    SERIALIZE_PIXELTYPE_NONE,
    SERIALIZE_PIXELTYPE_REF_DATA
};

void SkBitmap::flatten(SkFlattenableWriteBuffer& buffer) const {
    buffer.writeInt(fWidth);
    buffer.writeInt(fHeight);
    buffer.writeInt(fRowBytes);
    buffer.writeInt(fConfig);
    buffer.writeBool(this->isOpaque());

    if (fPixelRef) {
        if (fPixelRef->getFactory()) {
            buffer.writeInt(SERIALIZE_PIXELTYPE_REF_DATA);
            buffer.writeUInt(SkToU32(fPixelRefOffset));
            buffer.writeFlattenable(fPixelRef);
            return;
        }
        // if we get here, we can't record the pixels
        buffer.writeInt(SERIALIZE_PIXELTYPE_NONE);
    } else {
        buffer.writeInt(SERIALIZE_PIXELTYPE_NONE);
    }
}

void SkBitmap::unflatten(SkFlattenableReadBuffer& buffer) {
    this->reset();

    int width = buffer.readInt();
    int height = buffer.readInt();
    int rowBytes = buffer.readInt();
    int config = buffer.readInt();

    this->setConfig((Config)config, width, height, rowBytes);
    this->setIsOpaque(buffer.readBool());

    int reftype = buffer.readInt();
    switch (reftype) {
        case SERIALIZE_PIXELTYPE_REF_DATA: {
            size_t offset = buffer.readUInt();
            SkPixelRef* pr = buffer.readFlattenableT<SkPixelRef>();
            SkSafeUnref(this->setPixelRef(pr, offset));
            break;
        }
        case SERIALIZE_PIXELTYPE_NONE:
            break;
        default:
            SkDEBUGFAIL("unrecognized pixeltype in serialized data");
            sk_throw();
    }
}

///////////////////////////////////////////////////////////////////////////////

SkBitmap::RLEPixels::RLEPixels(int width, int height) {
    fHeight = height;
    fYPtrs = (uint8_t**)sk_malloc_throw(height * sizeof(uint8_t*));
    sk_bzero(fYPtrs, height * sizeof(uint8_t*));
}

SkBitmap::RLEPixels::~RLEPixels() {
    sk_free(fYPtrs);
}

///////////////////////////////////////////////////////////////////////////////

#ifdef SK_DEBUG
void SkBitmap::validate() const {
    SkASSERT(fConfig < kConfigCount);
    SkASSERT(fRowBytes >= (unsigned)ComputeRowBytes((Config)fConfig, fWidth));
    uint8_t allFlags = kImageIsOpaque_Flag | kImageIsVolatile_Flag | kImageIsImmutable_Flag;
#ifdef SK_BUILD_FOR_ANDROID
    allFlags |= kHasHardwareMipMap_Flag;
#endif
    SkASSERT(fFlags <= allFlags);
    SkASSERT(fPixelLockCount >= 0);
    SkASSERT(NULL == fColorTable || (unsigned)fColorTable->getRefCnt() < 10000);
    SkASSERT((uint8_t)ComputeBytesPerPixel((Config)fConfig) == fBytesPerPixel);

#if 0   // these asserts are not thread-correct, so disable for now
    if (fPixelRef) {
        if (fPixelLockCount > 0) {
            SkASSERT(fPixelRef->isLocked());
        } else {
            SkASSERT(NULL == fPixels);
            SkASSERT(NULL == fColorTable);
        }
    }
#endif
}
#endif

#ifdef SK_DEVELOPER
void SkBitmap::toString(SkString* str) const {

    static const char* gConfigNames[kConfigCount] = {
        "NONE", "A1", "A8", "INDEX8", "565", "4444", "8888"
    };

    str->appendf("bitmap: ((%d, %d) %s", this->width(), this->height(),
                 gConfigNames[this->config()]);

    str->append(" (");
    if (this->isOpaque()) {
        str->append("opaque");
    } else {
        str->append("transparent");
    }
    if (this->isImmutable()) {
        str->append(", immutable");
    } else {
        str->append(", not-immutable");
    }
    str->append(")");

    SkPixelRef* pr = this->pixelRef();
    if (NULL == pr) {
        // show null or the explicit pixel address (rare)
        str->appendf(" pixels:%p", this->getPixels());
    } else {
        const char* uri = pr->getURI();
        if (NULL != uri) {
            str->appendf(" uri:\"%s\"", uri);
        } else {
            str->appendf(" pixelref:%p", pr);
        }
    }

    str->append(")");
}
#endif
