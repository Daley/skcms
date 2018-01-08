/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <math.h>
#include <string.h>

static uint32_t make_signature(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)(a << 24)
         | (uint32_t)(b << 16)
         | (uint32_t)(c <<  8)
         | (uint32_t)(d <<  0);
}

static uint16_t read_big_u16(const uint8_t* ptr) {
    return (uint16_t)(ptr[0] << 8)
         | (uint16_t)(ptr[1] << 0);
}

static uint32_t read_big_u32(const uint8_t* ptr) {
    return (uint32_t)(ptr[0] << 24)
         | (uint32_t)(ptr[1] << 16)
         | (uint32_t)(ptr[2] <<  8)
         | (uint32_t)(ptr[3] <<  0);
}

static int32_t read_big_i32(const uint8_t* ptr) {
    return (int32_t)read_big_u32(ptr);
}

static uint64_t read_big_u64(const uint8_t* ptr) {
    uint64_t hi = read_big_u32(ptr);
    uint64_t lo = read_big_u32(ptr + 4);
    return hi << 32 | lo;
}

static float read_big_fixed(const uint8_t* ptr) {
    return read_big_i32(ptr) * (1.0f / 65536.0f);
}

static skcms_ICCDateTime read_big_date_time(const uint8_t* ptr) {
    skcms_ICCDateTime date_time;
    date_time.year   = read_big_u16(ptr + 0);
    date_time.month  = read_big_u16(ptr + 2);
    date_time.day    = read_big_u16(ptr + 4);
    date_time.hour   = read_big_u16(ptr + 6);
    date_time.minute = read_big_u16(ptr + 8);
    date_time.second = read_big_u16(ptr + 10);
    return date_time;
}

// Maps to an in-memory profile so that fields line up to the locations specified
// in ICC.1:2010, section 7.2
typedef struct {
    uint8_t size                [ 4];
    uint8_t cmm_type            [ 4];
    uint8_t version             [ 4];
    uint8_t profile_class       [ 4];
    uint8_t data_color_space    [ 4];
    uint8_t pcs                 [ 4];
    uint8_t creation_date_time  [12];
    uint8_t signature           [ 4];
    uint8_t platform            [ 4];
    uint8_t flags               [ 4];
    uint8_t device_manufacturer [ 4];
    uint8_t device_model        [ 4];
    uint8_t device_attributes   [ 8];
    uint8_t rendering_intent    [ 4];
    uint8_t illuminant_X        [ 4];
    uint8_t illuminant_Y        [ 4];
    uint8_t illuminant_Z        [ 4];
    uint8_t creator             [ 4];
    uint8_t profile_id          [16];
    uint8_t reserved            [28];
    uint8_t tag_count           [ 4]; // Technically not part of header, but required
} skcms_ICCHeader;

typedef struct {
    uint8_t signature [4];
    uint8_t offset    [4];
    uint8_t size      [4];
} skcms_ICCTag_Layout;

static const skcms_ICCTag_Layout* get_tag_table(const skcms_ICCProfile* profile) {
    return (const skcms_ICCTag_Layout*)(profile->buffer + sizeof(skcms_ICCHeader));
}

bool skcms_ICCProfile_parse(skcms_ICCProfile* profile,
                            const void* buf,
                            size_t len) {
    static_assert(sizeof(skcms_ICCHeader) == 132, "ICC header size");

    if (!profile) {
        return false;
    }
    memset(profile, 0, sizeof(*profile));

    if (len < sizeof(skcms_ICCHeader)) {
        return false;
    }

    // Byte-swap all header fields
    const skcms_ICCHeader* header = buf;
    profile->buffer              = buf;
    profile->size                = read_big_u32(header->size);
    profile->cmm_type            = read_big_u32(header->cmm_type);
    profile->version             = read_big_u32(header->version);
    profile->profile_class       = read_big_u32(header->profile_class);
    profile->data_color_space    = read_big_u32(header->data_color_space);
    profile->pcs                 = read_big_u32(header->pcs);
    profile->creation_date_time  = read_big_date_time(header->creation_date_time);
    profile->signature           = read_big_u32(header->signature);
    profile->platform            = read_big_u32(header->platform);
    profile->flags               = read_big_u32(header->flags);
    profile->device_manufacturer = read_big_u32(header->device_manufacturer);
    profile->device_model        = read_big_u32(header->device_model);
    profile->device_attributes   = read_big_u64(header->device_attributes);
    profile->rendering_intent    = read_big_u32(header->rendering_intent);
    profile->illuminant_X        = read_big_fixed(header->illuminant_X);
    profile->illuminant_Y        = read_big_fixed(header->illuminant_Y);
    profile->illuminant_Z        = read_big_fixed(header->illuminant_Z);
    profile->creator             = read_big_u32(header->creator);
    static_assert(sizeof(profile->profile_id) == sizeof(header->profile_id), "profile_id size");
    memcpy(profile->profile_id, header->profile_id, sizeof(header->profile_id));
    profile->tag_count           = read_big_u32(header->tag_count);

    // Validate signature, size (smaller than buffer, large enough to hold tag table),
    // and major version
    if (profile->signature != make_signature('a', 'c', 's', 'p') ||
        profile->size > len ||
        profile->size < sizeof(skcms_ICCHeader) + profile->tag_count*sizeof(skcms_ICCTag_Layout) ||
        (profile->version >> 24) > 4) {
        return false;
    }

    // Validate that illuminant is D50 white
    if (fabsf(profile->illuminant_X - 0.9642f) > 0.0100f ||
        fabsf(profile->illuminant_Y - 1.0000f) > 0.0100f ||
        fabsf(profile->illuminant_Z - 0.8249f) > 0.0100f) {
        return false;
    }

    // Validate that all tag entries have sane offset + size
    const skcms_ICCTag_Layout* tags = get_tag_table(profile);
    for (uint32_t i = 0; i < profile->tag_count; ++i) {
        uint32_t tag_offset = read_big_u32(tags[i].offset);
        uint32_t tag_size   = read_big_u32(tags[i].size);
        uint64_t tag_end    = (uint64_t)tag_offset + (uint64_t)tag_size;
        if (tag_end > profile->size) {
            return false;
        }
    }

    return true;
}

bool skcms_ICCProfile_toXYZD50(const skcms_ICCProfile* profile,
                               skcms_Matrix3x3* toXYZD50) {
    (void)profile;
    (void)toXYZD50;
    return false;
}

bool skcms_ICCTag_getParametricCurve(const skcms_ICCTag* tag,
                                     skcms_TransferFunction* curve) {
    if (!tag || tag->type != make_signature('p', 'a', 'r', 'a') || !curve) {
        return false;
    }

    enum { kG = 0, kGAB = 1, kGABC = 2, kGABCD = 3, kGABCDEF = 4 };
    uint16_t function_type = read_big_u16(tag->buf + 8);
    if (function_type > kGABCDEF) {
        return false;
    }

    static const uint32_t curve_bytes[] = { 4, 12, 16, 20, 28 };
    if (tag->size < 12U + curve_bytes[function_type]) {
        return false;
    }

    curve->a = 1.0f;
    curve->b = 0.0f;
    curve->c = 0.0f;
    curve->d = 0.0f;
    curve->e = 0.0f;
    curve->f = 0.0f;
    curve->g = read_big_fixed(tag->buf + 12);

    switch (function_type) {
        case kGAB:
            curve->a = read_big_fixed(tag->buf + 16);
            curve->b = read_big_fixed(tag->buf + 20);
            curve->d = -curve->b / curve->a;
            break;
        case kGABC:
            curve->a = read_big_fixed(tag->buf + 16);
            curve->b = read_big_fixed(tag->buf + 20);
            curve->e = read_big_fixed(tag->buf + 24);
            curve->d = -curve->b / curve->a;
            curve->f = curve->e;
            break;
        case kGABCD:
            curve->a = read_big_fixed(tag->buf + 16);
            curve->b = read_big_fixed(tag->buf + 20);
            curve->c = read_big_fixed(tag->buf + 24);
            curve->d = read_big_fixed(tag->buf + 28);
            break;
        case kGABCDEF:
            curve->a = read_big_fixed(tag->buf + 16);
            curve->b = read_big_fixed(tag->buf + 20);
            curve->c = read_big_fixed(tag->buf + 24);
            curve->e = read_big_fixed(tag->buf + 32);
            curve->f = read_big_fixed(tag->buf + 36);
            break;
    }
    return true;
}

bool skcms_ICCProfile_getTransferFunction(const skcms_ICCProfile* profile,
                                          skcms_TransferFunction* transferFunction) {
    if (!profile || !transferFunction) { return false; }
    skcms_ICCTag rTRC, gTRC, bTRC;
    // TODO: Skia code supported some of these being missing, with fallback to others!?
    if (!skcms_ICCProfile_getTagBySignature(profile, make_signature('r', 'T', 'R', 'C'), &rTRC) ||
        !skcms_ICCProfile_getTagBySignature(profile, make_signature('g', 'T', 'R', 'C'), &gTRC) ||
        !skcms_ICCProfile_getTagBySignature(profile, make_signature('b', 'T', 'R', 'C'), &bTRC)) {
        return false;
    }

    // TODO: Support 'curv' and do curve-fitting!
    skcms_TransferFunction rCurve, gCurve, bCurve;
    if (!skcms_ICCTag_getParametricCurve(&rTRC, &rCurve) ||
        !skcms_ICCTag_getParametricCurve(&gTRC, &gCurve) ||
        !skcms_ICCTag_getParametricCurve(&bTRC, &bCurve)) {
        return false;
    }

    if (memcmp(&rCurve, &gCurve, sizeof(rCurve)) || memcmp(&rCurve, &bCurve, sizeof(rCurve))) {
        return false;
    }

    *transferFunction = rCurve;
    return true;
}

void skcms_ICCProfile_getTagByIndex(const skcms_ICCProfile* profile,
                                    uint32_t index,
                                    skcms_ICCTag* tag) {
    if (!profile || !profile->buffer || !tag) { return; }
    if (index > profile->tag_count) { return; }
    const skcms_ICCTag_Layout* tags = get_tag_table(profile);
    tag->signature = read_big_u32(tags[index].signature);
    tag->size      = read_big_u32(tags[index].size);
    tag->buf       = read_big_u32(tags[index].offset) + profile->buffer;
    tag->type      = read_big_u32(tag->buf);
}

bool skcms_ICCProfile_getTagBySignature(const skcms_ICCProfile* profile,
                                        uint32_t signature,
                                        skcms_ICCTag* tag) {
    if (!profile || !profile->buffer || !tag) { return false; }
    const skcms_ICCTag_Layout* tags = get_tag_table(profile);
    for (uint32_t i = 0; i < profile->tag_count; ++i) {
        if (read_big_u32(tags[i].signature) == signature) {
            tag->signature = signature;
            tag->size      = read_big_u32(tags[i].size);
            tag->buf       = read_big_u32(tags[i].offset) + profile->buffer;
            tag->type      = read_big_u32(tag->buf);
            return true;
        }
    }
    return false;
}
