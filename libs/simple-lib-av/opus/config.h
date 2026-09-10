#pragma once

// What upstream opus expects a build system to have worked out for it. Written by hand because
// there is no build system here, only a project file.
//
// Fixed point, because nothing in the sound needs the accuracy of the other one and it leaves the
// whole floating point half of silk out of the build.

#define OPUS_BUILD      1
#define FIXED_POINT     1
#define DISABLE_FLOAT_API 1   // nothing here asks opus for samples with a decimal point

// how opus gets its working space. the alternative is a shared block of its own, which is not safe
// to use from more than one thread; this uses the stack instead.
#define VAR_ARRAYS      1

// the toolchain has both, declared in its own math.h. saying so stops opus falling back to a
// plain cast, which rounds differently.
#define HAVE_LRINT  1
#define HAVE_LRINTF 1

#define PACKAGE_VERSION "1.5.2"
#define OPUS_VERSION    "1.5.2"
