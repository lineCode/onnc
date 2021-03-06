//===- onnc/Support/Debug.h - Easy way to add debug output ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a handy way of adding debugging information to your
// code, without it being enabled all of the time, and without having to add
// command line options to enable it.
//
// In particular, just wrap your code with the DEBUG() macro, and it will be
// enabled automatically if you specify '-debug' on the command-line.
// DEBUG() requires the DEBUG_TYPE macro to be defined. Set it to "foo" specify
// that your debug code belongs to class "foo". Be careful that you only do
// this after including Debug.h and not around any #include of headers. Headers
// should define and undef the macro acround the code that needs to use the
// DEBUG() macro. Then, on the command line, you can specify '-debug-only=foo'
// to enable JUST the debug information for the foo class.
//
// When compiling without assertions, the -debug-* options and all code in
// DEBUG() statements disappears, so it does not affect the runtime of the code.
//
//===----------------------------------------------------------------------===//

#ifndef ONNC_SUPPORT_DEBUG_H
#define ONNC_SUPPORT_DEBUG_H

#include "onnc/Option/CommandLine.h"
#include "onnc/Support/IOStream.h"
#include <string>
#include <vector>

namespace onnc {

#ifndef NDEBUG

/// isCurrentDebugType - Return true if the specified string is the debug type
/// specified on the command line, or if none was specified on the command line
/// with the -debug-only=X option.
///
bool isCurrentDebugType(const char *Type);

/// setCurrentDebugType - Set the current debug type, as if the -debug-only=X
/// option were specified.  Note that DebugFlag also needs to be set to true for
/// debug output to be produced.
///
void setCurrentDebugType(const char *Type);

/// setCurrentDebugTypes - Set the current debug type, as if the
/// -debug-only=X,Y,Z option were specified. Note that DebugFlag
/// also needs to be set to true for debug output to be produced.
///
void setCurrentDebugTypes(const char **Types, unsigned Count);

/// DEBUG_WITH_TYPE macro - This macro should be used by passes to emit debug
/// information.  In the '-debug' option is specified on the commandline, and if
/// this is a debug build, then the code specified as the option to the macro
/// will be executed.  Otherwise it will not be.  Example:
///
/// DEBUG_WITH_TYPE("bitset", dbgs() << "Bitset contains: " << Bitset << "\n");
///
/// This will emit the debug information if -debug is present, and -debug-only
/// is not specified, or is specified as "bitset".
#define ENABLE_DEBUG()                                                         \
  do {                                                                         \
    DebugMsg d;                                                                \
  } while (false)
#define DEBUG_WITH_TYPE(TYPE, X)                                               \
  do {                                                                         \
    if (::onnc::DebugMsg::getDebugFlag() &&                                    \
        ::onnc::isCurrentDebugType(TYPE)) {                                    \
      X;                                                                       \
    }                                                                          \
  } while (false)

#else
#define isCurrentDebugType(X) (false)
#define setCurrentDebugType(X)
#define setCurrentDebugTypes(X, N)
#define DEBUG_WITH_TYPE(TYPE, X)                                               \
  do {                                                                         \
  } while (false)
#endif

/// dbgs() - This returns a reference to a raw_ostream for debugging
/// messages.  If debugging is disabled it returns errs().  Use it
/// like: dbgs() << "foo" << "bar";
onnc::OStream &dbgs();

// DEBUG macro - This macro should be used by passes to emit debug information.
// In the '-debug' option is specified on the commandline, and if this is a
// debug build, then the code specified as the option to the macro will be
// executed.  Otherwise it will not be.  Example:
//
// DEBUG(dbgs() << "Bitset contains: " << Bitset << "\n");
//
#define DEBUG(X) DEBUG_WITH_TYPE(DEBUG_TYPE, X)

class DebugMsg
{
public:
  DebugMsg();
  /// This boolean is set to true if the '-debug' command line option
  /// is specified.  This should probably not be referenced directly, instead,
  /// use the DEBUG macro below.
  static bool &getDebugFlag();
  static std::vector<std::string> *getCurrentDebugType();
  static void setDebugOnlyOpt(const std::string &pVal);
};
} // end namespace onnc

#endif // ONNC_SUPPORT_DEBUG_H
