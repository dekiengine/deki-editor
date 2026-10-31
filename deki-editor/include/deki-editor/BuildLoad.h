#pragma once

#include <deki-editor/EditorExport.h>

#include <string>

namespace DekiEditor
{

// Builds the editor starts share the machine with whatever else runs on it, a
// game or a call. So they run at below-normal priority: anything else gets
// the CPU first and the build takes what is spare, which on an idle machine is
// all of it. And they run as many compile jobs as free memory holds, not one
// per core, since one heavy C++ compile can take over a gigabyte.

/// Windows process creation flag for a build: below-normal priority, which the
/// processes it starts inherit. 0 elsewhere.
DEKI_EDITOR_API unsigned long BuildPriorityFlag();

/// Lowers the calling process's priority. For a build's child process, after
/// fork and before exec; what it starts inherits it. Does nothing on Windows.
DEKI_EDITOR_API void LowerBuildPriority();

/// A shell line run at build priority where the process cannot be lowered
/// before it starts (popen): `nice -n 10 /bin/sh -c '<line>'`. The line as
/// it is on Windows, which uses BuildPriorityFlag instead.
DEKI_EDITOR_API std::string AtBuildPriority(const std::string& shellLine);

/// How many compile jobs a build starting now should run: one per core, but
/// no more than free memory holds, and at least one.
DEKI_EDITOR_API int BuildJobCount();

/// Sets CMAKE_BUILD_PARALLEL_LEVEL (cmake --build) and IDF_PY_BUILD_JOBS
/// (idf.py) to BuildJobCount() for the processes this one starts next. A
/// value the user set before the editor started is left alone.
DEKI_EDITOR_API void PrepareBuildJobs();

}  // namespace DekiEditor
