/*
    Copyright (C) 2009 Mans Rullgard

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy,
    modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
 */

#ifndef OFBP_UTIL_H
#define OFBP_UTIL_H

#define ALIGN(n, a) (((n)+((a)-1))&~((a)-1))
#define MIN(a, b) ((a) < (b)? (a): (b))
#define MAX(a, b) ((a) > (b)? (a): (b))

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

#define TN(type, name) ofbp_##type##_##name
#define DRIVER(type, name)                                              \
    static const struct type TN(type, name);                            \
    static const struct type *const TN(type, name##_p)                  \
        __attribute__((section(".ofbp_"#type), used)) = &TN(type, name);\
    static const struct type TN(type, name)

#define OFBP_FULLSCREEN 1
#define OFBP_DOUBLE_BUF 2
#define OFBP_PHYS_MEM   4
#define OFBP_PRIV_MEM   8

#endif /* OFBP_UTIL_H */
