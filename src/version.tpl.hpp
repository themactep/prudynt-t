#ifndef VERSION_H_
#define VERSION_H_

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define COMPILE_TIME __DATE__ " " __TIME__
#define BUILD_COMMIT COMMIT_TAG

#define VERSION COMPILE_TIME "_" COMMIT_TAG
#define FULL_VERSION_STRING "prudynt-t " COMMIT_TAG " (" COMPILE_TIME ")"

#endif // VERSION_H_
