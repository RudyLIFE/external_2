
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "SkBitmapProcShader.h"
#include "SkColorPriv.h"
#include "SkFlattenableBuffers.h"
#include "SkPixelRef.h"
#include "SkErrorInternals.h"

bool SkBitmapProcShader::CanDo(const SkBitmap& bm, TileMode tx, TileMode ty) {
    switch (bm.config()) {
        case SkBitmap::kA8_Config:
        case SkBitmap::kRGB_565_Config:
        case SkBitmap::kIndex8_Config:
        case SkBitmap::kARGB_8888_Config:
    //        if (tx == ty && (kClamp_TileMode == tx || kRepeat_TileMode == tx))
                return true;
        default:
            break;
    }
    return false;
}

SkBitmapProcShader::SkBitmapProcShader(const SkBitmap& src,
                                       TileMode tmx, TileMode tmy) {
    fRawBitmap = src;
    fState.fTileModeX = (uint8_t)tmx;
    fState.fTileModeY = (uint8_t)tmy;
    fFlags = 0; // computed in setContext
}

SkBitmapProcShader::SkBitmapProcShader(SkFlattenableReadBuffer& buffer)
        : INHERITED(buffer) {
    buffer.readBitmap(&fRawBitmap);
    fRawBitmap.setImmutable();
    fState.fTileModeX = buffer.readUInt();
    fState.fTileModeY = buffer.readUInt();
    fFlags = 0; // computed in setContext
}

SkShader::BitmapType SkBitmapProcShader::asABitmap(SkBitmap* texture,
                                                   SkMatrix* texM,
                                                   TileMode xy[]) const {
    if (texture) {
        *texture = fRawBitmap;
    }
    if (texM) {
        texM->reset();
    }
    if (xy) {
        xy[0] = (TileMode)fState.fTileModeX;
        xy[1] = (TileMode)fState.fTileModeY;
    }
    return kDefault_BitmapType;
}

void SkBitmapProcShader::flatten(SkFlattenableWriteBuffer& buffer) const {
    this->INHERITED::flatten(buffer);

    buffer.writeBitmap(fRawBitmap);
    buffer.writeUInt(fState.fTileModeX);
    buffer.writeUInt(fState.fTileModeY);
}

static bool only_scale_and_translate(const SkMatrix& matrix) {
    unsigned mask = SkMatrix::kTranslate_Mask | SkMatrix::kScale_Mask;
    return (matrix.getType() & ~mask) == 0;
}

bool SkBitmapProcShader::isOpaque() const {
    return fRawBitmap.isOpaque();
}

bool SkBitmapProcShader::setContext(const SkBitmap& device,
                                    const SkPaint& paint,
                                    const SkMatrix& matrix) {
    // do this first, so we have a correct inverse matrix
    if (!this->INHERITED::setContext(device, paint, matrix)) {
        return false;
    }

    fState.fOrigBitmap = fRawBitmap;
    fState.fOrigBitmap.lockPixels();
    if (!fState.fOrigBitmap.getTexture() && !fState.fOrigBitmap.readyToDraw()) {
        fState.fOrigBitmap.unlockPixels();
        this->INHERITED::endContext();
        return false;
    }

    if (!fState.chooseProcs(this->getTotalInverse(), paint)) {
        fState.fOrigBitmap.unlockPixels();
        this->INHERITED::endContext();
        return false;
    }

    const SkBitmap& bitmap = *fState.fBitmap;
    bool bitmapIsOpaque = bitmap.isOpaque();

    // update fFlags
    uint32_t flags = 0;
    if (bitmapIsOpaque && (255 == this->getPaintAlpha())) {
        flags |= kOpaqueAlpha_Flag;
    }

    switch (bitmap.config()) {
        case SkBitmap::kRGB_565_Config:
            flags |= (kHasSpan16_Flag | kIntrinsicly16_Flag);
            break;
        case SkBitmap::kIndex8_Config:
        case SkBitmap::kARGB_8888_Config:
            if (bitmapIsOpaque) {
                flags |= kHasSpan16_Flag;
            }
            break;
        case SkBitmap::kA8_Config:
            break;  // never set kHasSpan16_Flag
        default:
            break;
    }

    if (paint.isDither() && bitmap.config() != SkBitmap::kRGB_565_Config) {
        // gradients can auto-dither in their 16bit sampler, but we don't so
        // we clear the flag here.
        flags &= ~kHasSpan16_Flag;
    }

    // if we're only 1-pixel high, and we don't rotate, then we can claim this
    if (1 == bitmap.height() &&
            only_scale_and_translate(this->getTotalInverse())) {
        flags |= kConstInY32_Flag;
        if (flags & kHasSpan16_Flag) {
            flags |= kConstInY16_Flag;
        }
    }

    fFlags = flags;
    return true;
}

void SkBitmapProcShader::endContext() {
    fState.fOrigBitmap.unlockPixels();
    fState.endContext();
    this->INHERITED::endContext();
}

#define BUF_MAX     128

#define TEST_BUFFER_OVERRITEx

#ifdef TEST_BUFFER_OVERRITE
    #define TEST_BUFFER_EXTRA   32
    #define TEST_PATTERN    0x88888888
#else
    #define TEST_BUFFER_EXTRA   0
#endif

extern void ClampX_ClampY_nofilter_scale_neon(const SkBitmapProcState& s,
                                uint32_t xy[], int count, int x, int y);
extern void S32_opaque_D32_nofilter_DX_neon(const SkBitmapProcState& s,
                            const uint32_t* SK_RESTRICT xy,
                            int count, SkPMColor* SK_RESTRICT colors);

void SkBitmapProcShader::shadeSpan(int x, int y, SkPMColor dstC[], int count) {
    const SkBitmapProcState& state = fState;
    if (state.getShaderProc32()) {
        state.getShaderProc32()(state, x, y, dstC, count);
        return;
    }

    uint32_t buffer[BUF_MAX + TEST_BUFFER_EXTRA];
    SkBitmapProcState::MatrixProc   mproc = state.getMatrixProc();
    SkBitmapProcState::SampleProc32 sproc = state.getSampleProc32();
    int max = fState.maxCountForBufferSize(sizeof(buffer[0]) * BUF_MAX);

    SkASSERT(state.fBitmap->getPixels());
    SkASSERT(state.fBitmap->pixelRef() == NULL ||
             state.fBitmap->pixelRef()->isLocked());

    for (;;) {
        int n = count;
        if (n > max) {
            n = max;
        }
        SkASSERT(n > 0 && n < BUF_MAX*2);
#ifdef TEST_BUFFER_OVERRITE
        for (int i = 0; i < TEST_BUFFER_EXTRA; i++) {
            buffer[BUF_MAX + i] = TEST_PATTERN;
        }
#endif
        mproc(state, buffer, n, x, y);
#ifdef TEST_BUFFER_OVERRITE
        for (int j = 0; j < TEST_BUFFER_EXTRA; j++) {
            SkASSERT(buffer[BUF_MAX + j] == TEST_PATTERN);
        }
#endif
        sproc(state, buffer, n, dstC);

        if ((count -= n) == 0) {
            break;
        }
        SkASSERT(count > 0);
        x += n;
        dstC += n;
    }
}

bool SkBitmapProcShader::shadeSpanRect(int x, int y, SkPMColor dstC[], int rb, int count, int height) {

    const SkBitmapProcState& state = fState;
    SkBitmapProcState::MatrixProc   mproc = state.getMatrixProc();
    SkBitmapProcState::SampleProc32 sproc = state.getSampleProc32();
#if !defined(__ARM_HAVE_NEON)
    return false;
#else
    if ( (ClampX_ClampY_nofilter_scale_neon != mproc) || (S32_opaque_D32_nofilter_DX_neon != sproc) )
    {
        return false;
    }

    uint32_t *buffer = (uint32_t*)malloc((count/2+2) * sizeof(uint32_t));
    if (buffer == NULL)
    {
        SkDebugf("insufficient memory in %s", __func__);
        return false;
    }
    
    SkASSERT(state.fBitmap->getPixels());
    SkASSERT(state.fBitmap->pixelRef() == NULL ||
             state.fBitmap->pixelRef()->isLocked());

    SkFixed fy;
    const unsigned maxY = state.fBitmap->height() - 1;

    SkPoint pt;
    state.fInvProc(state.fInvMatrix, SkIntToScalar(x) + SK_ScalarHalf,
            SkIntToScalar(y) + SK_ScalarHalf, &pt);
    fy = SkScalarToFixed(pt.fY);
    fy = SkClampMax((fy) >> 16, maxY);

    
    SkFixed dy = state.fInvKy;
    mproc(state, buffer, count, x, y);
    while (height > 0) {
        sproc(state, buffer, count, dstC);

        y++;
        state.fInvProc(state.fInvMatrix, SkIntToScalar(x) + SK_ScalarHalf,
                SkIntToScalar(y) + SK_ScalarHalf, &pt);
        fy = SkScalarToFixed(pt.fY);
        buffer[0] = SkClampMax((fy) >> 16, maxY);

        dstC = (SkPMColor*)((char*)dstC + rb);
        height--;
    }
    free(buffer);

    return true;

#endif
}

SkShader::ShadeProc SkBitmapProcShader::asAShadeProc(void** ctx) {
    if (fState.getShaderProc32()) {
        *ctx = &fState;
        return (ShadeProc)fState.getShaderProc32();
    }
    return NULL;
}

void SkBitmapProcShader::shadeSpan16(int x, int y, uint16_t dstC[], int count) {
    const SkBitmapProcState& state = fState;
    if (state.getShaderProc16()) {
        state.getShaderProc16()(state, x, y, dstC, count);
        return;
    }

    uint32_t buffer[BUF_MAX];
    SkBitmapProcState::MatrixProc   mproc = state.getMatrixProc();
    SkBitmapProcState::SampleProc16 sproc = state.getSampleProc16();
    int max = fState.maxCountForBufferSize(sizeof(buffer));

    SkASSERT(state.fBitmap->getPixels());
    SkASSERT(state.fBitmap->pixelRef() == NULL ||
             state.fBitmap->pixelRef()->isLocked());

    for (;;) {
        int n = count;
        if (n > max) {
            n = max;
        }
        mproc(state, buffer, n, x, y);
        sproc(state, buffer, n, dstC);

        if ((count -= n) == 0) {
            break;
        }
        x += n;
        dstC += n;
    }
}

///////////////////////////////////////////////////////////////////////////////

#include "SkUnPreMultiply.h"
#include "SkColorShader.h"
#include "SkEmptyShader.h"

// returns true and set color if the bitmap can be drawn as a single color
// (for efficiency)
static bool canUseColorShader(const SkBitmap& bm, SkColor* color) {
    if (1 != bm.width() || 1 != bm.height()) {
        return false;
    }

    SkAutoLockPixels alp(bm);
    if (!bm.readyToDraw()) {
        return false;
    }

    switch (bm.config()) {
        case SkBitmap::kARGB_8888_Config:
            *color = SkUnPreMultiply::PMColorToColor(*bm.getAddr32(0, 0));
            return true;
        case SkBitmap::kRGB_565_Config:
            *color = SkPixel16ToColor(*bm.getAddr16(0, 0));
            return true;
        case SkBitmap::kIndex8_Config:
            *color = SkUnPreMultiply::PMColorToColor(bm.getIndex8Color(0, 0));
            return true;
        default: // just skip the other configs for now
            break;
    }
    return false;
}

#include "SkTemplatesPriv.h"

static bool bitmapIsTooBig(const SkBitmap& bm) {
    // SkBitmapProcShader stores bitmap coordinates in a 16bit buffer, as it
    // communicates between its matrix-proc and its sampler-proc. Until we can
    // widen that, we have to reject bitmaps that are larger.
    //
    const int maxSize = 65535;

    return bm.width() > maxSize || bm.height() > maxSize;
}

SkShader* SkShader::CreateBitmapShader(const SkBitmap& src,
                                       TileMode tmx, TileMode tmy,
                                       void* storage, size_t storageSize) {
    SkShader* shader;
    SkColor color;
    if (src.isNull() || bitmapIsTooBig(src)) {
        SK_PLACEMENT_NEW(shader, SkEmptyShader, storage, storageSize);
    }
    else if (canUseColorShader(src, &color)) {
        SK_PLACEMENT_NEW_ARGS(shader, SkColorShader, storage, storageSize,
                              (color));
    } else {
        SK_PLACEMENT_NEW_ARGS(shader, SkBitmapProcShader, storage,
                              storageSize, (src, tmx, tmy));
    }
    return shader;
}

///////////////////////////////////////////////////////////////////////////////

#ifdef SK_DEVELOPER
void SkBitmapProcShader::toString(SkString* str) const {
    static const char* gTileModeName[SkShader::kTileModeCount] = {
        "clamp", "repeat", "mirror"
    };

    str->append("BitmapShader: (");

    str->appendf("(%s, %s)",
                 gTileModeName[fState.fTileModeX],
                 gTileModeName[fState.fTileModeY]);

    str->append(" ");
    fRawBitmap.toString(str);

    this->INHERITED::toString(str);

    str->append(")");
}
#endif

///////////////////////////////////////////////////////////////////////////////

#if SK_SUPPORT_GPU

#include "GrTextureAccess.h"
#include "effects/GrSimpleTextureEffect.h"
#include "SkGr.h"

GrEffectRef* SkBitmapProcShader::asNewEffect(GrContext* context, const SkPaint& paint) const {
    SkMatrix matrix;
    matrix.setIDiv(fRawBitmap.width(), fRawBitmap.height());

    if (this->hasLocalMatrix()) {
        SkMatrix inverse;
        if (!this->getLocalMatrix().invert(&inverse)) {
            return NULL;
        }
        matrix.preConcat(inverse);
    }
    SkShader::TileMode tm[] = {
        (TileMode)fState.fTileModeX,
        (TileMode)fState.fTileModeY,
    };

    // Must set wrap and filter on the sampler before requesting a texture.
    SkPaint::FilterLevel paintFilterLevel = paint.getFilterLevel();
    GrTextureParams::FilterMode textureFilterMode;
    switch(paintFilterLevel) {
        case SkPaint::kNone_FilterLevel:
            textureFilterMode = GrTextureParams::kNone_FilterMode;
            break;
        case SkPaint::kLow_FilterLevel:
            textureFilterMode = GrTextureParams::kBilerp_FilterMode;
            break;
        case SkPaint::kMedium_FilterLevel:
            textureFilterMode = GrTextureParams::kMipMap_FilterMode;
            break;
        case SkPaint::kHigh_FilterLevel:
            SkErrorInternals::SetError( kInvalidPaint_SkError,
                                        "Sorry, I don't yet support high quality "
                                        "filtering on the GPU; falling back to "
                                        "MIPMaps.");
            textureFilterMode = GrTextureParams::kMipMap_FilterMode;
            break;
        default:
            SkErrorInternals::SetError( kInvalidPaint_SkError,
                                        "Sorry, I don't understand the filtering "
                                        "mode you asked for.  Falling back to "
                                        "MIPMaps.");
            textureFilterMode = GrTextureParams::kMipMap_FilterMode;
            break;

    }
    GrTextureParams params(tm, textureFilterMode);
    GrTexture* texture = GrLockAndRefCachedBitmapTexture(context, fRawBitmap, &params);

    if (NULL == texture) {
        SkDebugf("Couldn't convert bitmap to texture.\n");
        return NULL;
    }

    GrEffectRef* effect = GrSimpleTextureEffect::Create(texture, matrix, params);
    GrUnlockAndUnrefCachedBitmapTexture(texture);
    return effect;
}
#endif
