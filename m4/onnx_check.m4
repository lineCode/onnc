dnl
dnl @synopsis CHECK_ONNX
dnl
dnl @summary check --with-onnx=<installdir>, and setup variables:
dnl   ONNX_INCLUDES="-I${PYTHON_PATH}"
dnl   ONNX_LIBS="${onnx_dir}/lib/"
dnl
dnl
AC_DEFUN([CHECK_ONNX],
[dnl

AC_ARG_WITH([onnx],
  [AS_HELP_STRING([--with-onnx@<:@=DIR@:>@],
    [use ONNX on DIR, or PREFIX if not given])],
  [onnx_dir="${withval}"],
  [onnx_dir="${prefix}"])

AC_MSG_CHECKING(ONNX)

HAVE_ONNX=0

AC_LANG_PUSH([C++])
orig_CXXFLAGS="${CXXFLAGS}"

PYTHON_PATH="/usr/local/lib/python2.7/dist-packages"
dnl TODO
dnl pyhton_path = esyscmd(`python -c \"import onnx, os; print(os.path.dirname(onnx.__path__[0]))\"')
CXXFLAGS="-I${PYTHON_PATH} -DONNX_NAMESPACE=onnx"

AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
    #include <onnx/onnx_pb.h>
  ]], [])],
  [dnl action if true
    HAVE_ONNX=1
    AC_MSG_RESULT([ok])
  ],
  [dnl action if else
    AC_MSG_FAILURE([failed to find ONNX headers])
  ]
)

CXXFLAGS="${orig_CXXFLAGS}"
AC_LANG_POP([C++])

ONNX_INCLUDES="-I${PYTHON_PATH} -DONNX_NAMESPACE=onnx"
ONNX_LIBS="-L${onnx_dir}/lib -lonnx -lprotobuf -lpython2.7"

AC_SUBST(ONNX_INCLUDES)
AC_SUBST(ONNX_LIBS)

AM_CONDITIONAL([HAVE_ONNX], [test ${HAVE_ONNX} = 1])
])