/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS
    #define SKCMS_NORETURN __declspec(noreturn)
#else
    #include <stdnoreturn.h>
    #define SKCMS_NORETURN noreturn
#endif

#include "skcms.h"
#include "test_only.h"
#include "src/Macros.h"
#include "src/TransferFunction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SKCMS_NORETURN
static void fatal(const char* msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(1);
}

// xy co-ordinates of the CIE 1931 standard observer XYZ functions.
// wavelength is sampled every 5 nm in [360, 700].
// This is effectively the hull of the horseshoe in a chromaticity diagram.
static const double kSpectralHull[] = {
    0.17556, 0.00529384,
    0.175161, 0.00525635,
    0.174821, 0.0052206,
    0.17451, 0.00518164,
    0.174112, 0.00496373,
    0.174008, 0.00498055,
    0.173801, 0.00491541,
    0.17356, 0.0049232,
    0.173337, 0.00479674,
    0.173021, 0.00477505,
    0.172577, 0.0047993,
    0.172087, 0.00483252,
    0.171407, 0.00510217,
    0.170301, 0.00578851,
    0.168878, 0.00690024,
    0.166895, 0.00855561,
    0.164412, 0.0108576,
    0.161105, 0.0137934,
    0.156641, 0.0177048,
    0.150985, 0.0227402,
    0.14396, 0.029703,
    0.135503, 0.0398791,
    0.124118, 0.0578025,
    0.109594, 0.0868425,
    0.0912935, 0.132702,
    0.0687059, 0.200723,
    0.0453907, 0.294976,
    0.0234599, 0.412703,
    0.00816803, 0.538423,
    0.00385852, 0.654823,
    0.0138702, 0.750186,
    0.0388518, 0.812016,
    0.0743024, 0.833803,
    0.114161, 0.826207,
    0.154722, 0.805863,
    0.192876, 0.781629,
    0.22962, 0.754329,
    0.265775, 0.724324,
    0.301604, 0.692308,
    0.337363, 0.658848,
    0.373102, 0.624451,
    0.408736, 0.589607,
    0.444062, 0.554714,
    0.478775, 0.520202,
    0.512486, 0.486591,
    0.544787, 0.454434,
    0.575151, 0.424232,
    0.602933, 0.396497,
    0.627037, 0.372491,
    0.648233, 0.351395,
    0.665764, 0.334011,
    0.680079, 0.319747,
    0.691504, 0.308342,
    0.700606, 0.299301,
    0.707918, 0.292027,
    0.714032, 0.285929,
    0.719033, 0.280935,
    0.723032, 0.276948,
    0.725992, 0.274008,
    0.728272, 0.271728,
    0.729969, 0.270031,
    0.731089, 0.268911,
    0.731993, 0.268007,
    0.732719, 0.267281,
    0.733417, 0.266583,
    0.734047, 0.265953,
    0.73439, 0.26561,
    0.734592, 0.265408,
    0.73469, 0.26531,
};

static uint16_t read_big_u16(const uint8_t* ptr) {
    uint16_t be;
    memcpy(&be, ptr, sizeof(be));
#if defined(_MSC_VER)
    return _byteswap_ushort(be);
#else
    return __builtin_bswap16(be);
#endif
}

// TODO: Put state into struct with FP
static int desmos_id = 0;

static FILE* desmos_open(const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fatal("Unable to open output file");
    }

    fprintf(fp, "<!DOCTYPE html>\n");
    fprintf(fp, "<html>\n");
    fprintf(fp, "<head>\n");
    fprintf(fp, "<script src=\"https://www.desmos.com/api/v1.1/calculator.js?apiKey=dcb31709b452b1cf9dc26972add0fda6\"></script>\n");
    fprintf(fp, "<style>\n");
    fprintf(fp, "  html, body{ width: 100%%; height: 100%%; margin: 0; padding: 0; overflow: hidden; }\n");
    fprintf(fp, "  #calculator { width: 100%%; height: 100%%; }\n");
    fprintf(fp, "</style>\n");
    fprintf(fp, "</head>\n");
    fprintf(fp, "<body>\n");
    fprintf(fp, "<div id=\"calculator\"></div>\n");
    fprintf(fp, "<script>\n");
    fprintf(fp, "var elt = document.getElementById('calculator');\n");
    fprintf(fp, "var c = Desmos.GraphingCalculator(elt);\n");
    fprintf(fp, "c.setState({\n");
    fprintf(fp, "\"version\": 5,\n");
    fprintf(fp, "\"expressions\": {\n");
    fprintf(fp, "\"list\": [\n");

    desmos_id = 0;
    return fp;
}

static void desmos_close(FILE* fp) {
    fprintf(fp, "] } } );\n");
    fprintf(fp, "c.setMathBounds({left: -0.1, right: 1.1, bottom: -0.1, top: 1.1});\n");
    fprintf(fp, "</script>\n");
    fprintf(fp, "</body>\n");
    fprintf(fp, "</html>\n");
    fclose(fp);
}

static void desmos_transfer_function(FILE* fp, const skcms_TransferFunction* tf,
                                     const char* color) {
    fprintf(fp, "{\n");
    fprintf(fp, " \"type\": \"expression\",\n");
    fprintf(fp, " \"id\": \"%d\",\n", desmos_id++);
    fprintf(fp, " \"color\": \"%s\",\n", color);
    fprintf(fp, " \"latex\": \"\\\\left\\\\{"
            "0 \\\\le x < %.5f: %.5fx + %.5f, "                    // 0 <= x < d: cx + f
            "%.5f \\\\le x \\\\le 1: (%.5fx + %.5f)^{%.5f} + %.5f" // d <= x <= 1: (ax + b)^g + e
            "\\\\right\\\\}\"\n",
            (double)tf->d, (double)tf->c, (double)tf->f,
            (double)tf->d, (double)tf->a, (double)tf->b, (double)tf->g, (double)tf->e);
    fprintf(fp, "},\n");
}

static void desmos_TF13(FILE* fp, const skcms_TF13* tf, const char* color) {
    double A = tf->A,
           B = tf->B;
    fprintf(fp, "{\n");
    fprintf(fp, " \"type\": \"expression\",\n");
    fprintf(fp, " \"id\": \"%d\",\n", desmos_id++);
    fprintf(fp, " \"color\": \"%s\",\n", color);
    fprintf(fp, " \"latex\": \"%.5fx^3 + %.5fx^2 + %.5fx"
            "\\\\left\\\\{0 \\\\le x \\\\le 1 \\\\right\\\\}\"\n", A, B, (1 - A - B));
    fprintf(fp, "},\n");
}

static void desmos_curve(FILE* fp, const skcms_Curve* curve, const char* color) {
    if (!curve->table_entries) {
        desmos_transfer_function(fp, &curve->parametric, color);
        return;
    }

    int folder_id = desmos_id++,
        table_id  = desmos_id++;

    // Folder
    fprintf(fp, "{\n");
    fprintf(fp, " \"type\": \"folder\",\n");
    fprintf(fp, " \"id\": \"%d\",\n", folder_id);
    fprintf(fp, " \"title\": \"%s Table\",\n", color);
    fprintf(fp, " \"collapsed\": true,\n");
    fprintf(fp, " \"memberIds\": { \"%d\": true }\n", table_id);
    fprintf(fp, "},\n");

    // Table
    fprintf(fp, "{\n");
    fprintf(fp, " \"type\": \"table\",\n");
    fprintf(fp, " \"id\": \"%d\",\n", table_id);
    fprintf(fp, " \"columns\": [\n");

    double xScale = 1.0 / (curve->table_entries - 1.0);

    // X Column
    fprintf(fp, " {\n");
    fprintf(fp, "  \"values\": [");

    for (uint32_t i = 0; i < curve->table_entries; ++i) {
        if (i % 6 == 0) {
            fprintf(fp, "\n  ");
        }
        fprintf(fp, " \"%.5f\",", xScale * i);
    }

    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"hidden\": true,\n");
    fprintf(fp, "  \"id\": \"%d\",\n", desmos_id++);
    fprintf(fp, "  \"color\": \"%s\",\n", color);
    fprintf(fp, "  \"latex\": \"x_%c\"\n", color[0]);
    fprintf(fp, " },\n");

    // Y Column
    fprintf(fp, " {\n");
    fprintf(fp, "  \"values\": [\n");

    for (uint32_t i = 0; i < curve->table_entries; ++i) {
        if (i % 6 == 0) {
            fprintf(fp, "\n  ");
        }
        fprintf(fp, " \"%.5f\",",
                curve->table_8 ? curve->table_8[i] / 255.0
                               : read_big_u16(curve->table_16 + 2 * i) / 65535.0);
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"id\": \"%d\",\n", desmos_id++);
    fprintf(fp, "  \"color\": \"%s\",\n", color);
    fprintf(fp, "  \"latex\": \"y_%c\"\n", color[0]);
    fprintf(fp, " }\n");
    fprintf(fp, " ]\n");
    fprintf(fp, "},\n");


    char approx_color[64];
    (void)snprintf(approx_color, sizeof(approx_color), "Dark%s", color);

    skcms_TransferFunction approx_tf;
    float max_error;
    if (skcms_ApproximateCurve(curve, &approx_tf, &max_error)) {
        desmos_transfer_function(fp, &approx_tf, approx_color);
    }
    skcms_TF13 tf13;
    if (skcms_ApproximateCurve13(curve, &tf13, &max_error)) {
        desmos_TF13(fp, &tf13, approx_color);
    }
}

static void desmos_curves(FILE* fp, uint32_t num_curves, const skcms_Curve* curves,
                          const char** colors) {
    for (uint32_t c = 0; c < num_curves; ++c) {
        desmos_curve(fp, curves + c, colors[c]);
    }
}

static const double kSVGMarginLeft   = 100.0;
static const double kSVGMarginRight  = 10.0;
static const double kSVGMarginTop    = 10.0;
static const double kSVGMarginBottom = 50.0;

static const double kSVGScaleX = 800.0;
static const double kSVGScaleY = 800.0;

static const char* kSVG_RGB_Colors[3] = { "Red", "Green", "Blue" };
static const char* kSVG_CMYK_Colors[4] = { "cyan", "magenta", "yellow", "black" };

static FILE* svg_open(const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fatal("Unable to open output file");
    }

    fprintf(fp, "<svg width=\"%g\" height=\"%g\" xmlns=\"http://www.w3.org/2000/svg\">\n",
            kSVGMarginLeft + kSVGScaleX + kSVGMarginRight,
            kSVGMarginTop + kSVGScaleY + kSVGMarginBottom);
    return fp;
}

static void svg_close(FILE* fp) {
    fprintf(fp, "</svg>\n");
    fclose(fp);
}

#define svg_push_group(fp, fmt, ...) fprintf(fp, "<g " fmt ">\n", __VA_ARGS__)

static void svg_pop_group(FILE* fp) {
    fprintf(fp, "</g>\n");
}

static void svg_axes(FILE* fp) {
    fprintf(fp, "<polyline fill=\"none\" stroke=\"black\" vector-effect=\"non-scaling-stroke\" "
                "points=\"0,1 0,0 1,0\"/>\n");
}

static void svg_transfer_function(FILE* fp, const skcms_TransferFunction* tf, const char* color) {
    fprintf(fp, "<polyline fill=\"none\" stroke=\"%s\" vector-effect=\"non-scaling-stroke\" "
            "points=\"\n", color);

    for (int i = 0; i < 256; ++i) {
        float x = i / 255.0f;
        float t = skcms_TransferFunction_eval(tf, x);
        fprintf(fp, "%g, %g\n", (double)x, (double)t);
    }
    fprintf(fp, "\"/>\n");
}

static void svg_curve(FILE* fp, const skcms_Curve* curve, const char* color) {
    if (!curve->table_entries) {
        svg_transfer_function(fp, &curve->parametric, color);
        return;
    }

    double xScale = 1.0 / (curve->table_entries - 1.0);
    double yScale = curve->table_8 ? (1.0 / 255) : (1.0 / 65535);
    fprintf(fp, "<polyline fill=\"none\" stroke=\"%s\" vector-effect=\"non-scaling-stroke\" "
            "transform=\"scale(%g %g)\" points=\"\n",
            color, xScale, yScale);

    for (uint32_t i = 0; i < curve->table_entries; ++i) {
        if (curve->table_8) {
            fprintf(fp, "%3u, %3u\n", i, curve->table_8[i]);
        } else {
            fprintf(fp, "%4u, %5u\n", i, read_big_u16(curve->table_16 + 2 * i));
        }
    }
    fprintf(fp, "\"/>\n");

    skcms_TransferFunction approx_tf;
    float max_error;
    if (skcms_ApproximateCurve(curve, &approx_tf, &max_error)) {
        svg_transfer_function(fp, &approx_tf, "magenta");
    }
}

static void svg_curves(FILE* fp, uint32_t num_curves, const skcms_Curve* curves,
                       const char** colors) {
    for (uint32_t c = 0; c < num_curves; ++c) {
        svg_curve(fp, curves + c, colors[c]);
    }
}

static void dump_curves_svg(const char* filename, uint32_t num_curves, const skcms_Curve* curves) {
    FILE* fp = svg_open(filename);
    svg_push_group(fp, "transform=\"translate(%g %g) scale(%g %g)\"",
                   kSVGMarginLeft, kSVGMarginTop + kSVGScaleY, kSVGScaleX, -kSVGScaleY);
    svg_axes(fp);
    svg_curves(fp, num_curves, curves, (num_curves == 3) ? kSVG_RGB_Colors : kSVG_CMYK_Colors);
    svg_pop_group(fp);
    svg_close(fp);
}

int main(int argc, char** argv) {
    const char* filename = NULL;
    bool svg = false;
    bool desmos = false;

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "-s")) {
            svg = true;
        } else if (0 == strcmp(argv[i], "-d")) {
            desmos = true;
        } else {
            filename = argv[i];
        }
    }

    if (!filename) {
        printf("usage: %s [-s] <ICC filename>\n", argv[0]);
        return 1;
    }

    void* buf = NULL;
    size_t len = 0;
    if (!load_file(filename, &buf, &len)) {
        fatal("Unable to load input file");
    }

    skcms_ICCProfile profile;
    if (!skcms_Parse(buf, len, &profile)) {
        fatal("Unable to parse ICC profile");
    }

    dump_profile(&profile, stdout, false);

    if (desmos) {
        if (profile.has_trc) {
            FILE* fp = desmos_open("TRC_curves.html");
            desmos_curves(fp, 3, profile.trc, kSVG_RGB_Colors);
            desmos_close(fp);
        }
    }

    if (svg) {
        if (profile.has_toXYZD50) {
            FILE* fp = svg_open("gamut.svg");
            svg_push_group(fp, "transform=\"translate(%g %g) scale(%g %g)\"",
                           kSVGMarginLeft, kSVGMarginTop + kSVGScaleY, kSVGScaleX, -kSVGScaleY);
            svg_axes(fp);

            fprintf(fp, "<polygon fill=\"none\" stroke=\"black\" "
                    "vector-effect=\"non-scaling-stroke\" points=\"\n");
            for (int i = 0; i < ARRAY_COUNT(kSpectralHull); i += 2) {
                fprintf(fp, "%g, %g\n", kSpectralHull[i], kSpectralHull[i + 1]);
            }
            fprintf(fp, "\"/>\n");

            const skcms_Matrix3x3* m = &profile.toXYZD50;
            float rSum = m->vals[0][0] + m->vals[1][0] + m->vals[2][0];
            float gSum = m->vals[0][1] + m->vals[1][1] + m->vals[2][1];
            float bSum = m->vals[0][2] + m->vals[1][2] + m->vals[2][2];
            fprintf(fp, "<polygon fill=\"none\" stroke=\"black\" "
                    "vector-effect=\"non-scaling-stroke\" points=\"%g,%g %g,%g %g,%g\"/>\n",
                    (double)(m->vals[0][0] / rSum), (double)(m->vals[1][0] / rSum),
                    (double)(m->vals[0][1] / gSum), (double)(m->vals[1][1] / gSum),
                    (double)(m->vals[0][2] / bSum), (double)(m->vals[1][2] / bSum));

            svg_pop_group(fp);
            svg_close(fp);
        }

        if (profile.has_trc) {
            FILE* fp = svg_open("TRC_curves.svg");
            svg_push_group(fp, "transform=\"translate(%g %g) scale(%g %g)\"",
                           kSVGMarginLeft, kSVGMarginTop + kSVGScaleY, kSVGScaleX, -kSVGScaleY);
            svg_axes(fp);
            svg_curves(fp, 3, profile.trc, kSVG_RGB_Colors);
            svg_pop_group(fp);
            svg_close(fp);
        }

        if (profile.has_A2B) {
            const skcms_A2B* a2b = &profile.A2B;
            if (a2b->input_channels) {
                dump_curves_svg("A_curves.svg", a2b->input_channels, a2b->input_curves);
            }

            if (a2b->matrix_channels) {
                dump_curves_svg("M_curves.svg", a2b->matrix_channels, a2b->matrix_curves);
            }

            dump_curves_svg("B_curves.svg", a2b->output_channels, a2b->output_curves);
        }
    }

    return 0;
}
