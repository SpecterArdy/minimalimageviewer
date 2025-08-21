#pragma once

// Optional OpenColorIO shim.
// If PORTABLE_NO_OCIO is defined, provide minimal stand-ins so the app can
// compile and run without the OpenColorIO runtime DLL. Otherwise, include
// the real OCIO headers.

#ifdef PORTABLE_NO_OCIO

#include <exception>

namespace OCIO {

// Minimal stand-ins to satisfy type usage in this app
using ConstConfigRcPtr = void*;
using ConstProcessorRcPtr = void*;

struct Config {
    static ConstConfigRcPtr CreateRaw() { return nullptr; }
    int getNumColorSpaces() const { return 0; }
};

inline ConstConfigRcPtr GetCurrentConfig() { return nullptr; }

class Exception : public std::exception {
public:
    const char* what() const noexcept override { return "OCIO disabled"; }
};

} // namespace OCIO

#else

#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;

#endif
