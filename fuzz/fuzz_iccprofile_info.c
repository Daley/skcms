/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// This fuzz target parses an ICCProfile and then queries several pieces
// of info from it.

#include "../skcms.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    skcms_ICCProfile p;
    if (!skcms_Parse(data, size, &p)) {
        return 0;
    }

    // Instead of testing all tags, just test that we can read the first and last.
    // This does _not_ imply all the middle will work fine, but these calls should
    // be enough for the fuzzer to find a way to break us.
    if (p.tag_count > 0) {
        skcms_ICCTag tag;
        skcms_GetTagByIndex(&p,               0, &tag);
        skcms_GetTagByIndex(&p, p.tag_count - 1, &tag);
    }

    skcms_A2B a2b;
    skcms_GetA2B(&p, &a2b);

    return 0;
}
