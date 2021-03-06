/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmCMakePresetsFile.h"

#include <cstdlib>
#include <functional>
#include <utility>

#include <cmext/string_view>

#include <cm3p/json/reader.h>
#include <cm3p/json/value.h>

#include "cmsys/FStream.hxx"

#include "cmJSONHelpers.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmVersion.h"

namespace {
enum class CycleStatus
{
  Unvisited,
  InProgress,
  Verified,
};

using ReadFileResult = cmCMakePresetsFile::ReadFileResult;
using CacheVariable = cmCMakePresetsFile::CacheVariable;
using UnexpandedPreset = cmCMakePresetsFile::UnexpandedPreset;
using CMakeGeneratorConfig = cmCMakePresetsFile::CMakeGeneratorConfig;

constexpr int MIN_VERSION = 1;
constexpr int MAX_VERSION = 1;

struct CMakeVersion
{
  unsigned int Major = 0;
  unsigned int Minor = 0;
  unsigned int Patch = 0;
};

struct RootPresets
{
  CMakeVersion CMakeMinimumRequired;
  std::vector<cmCMakePresetsFile::UnexpandedPreset> Presets;
};

cmJSONHelper<std::nullptr_t, ReadFileResult> VendorHelper(ReadFileResult error)
{
  return [error](std::nullptr_t& /*out*/,
                 const Json::Value* value) -> ReadFileResult {
    if (!value) {
      return ReadFileResult::READ_OK;
    }

    if (!value->isObject()) {
      return error;
    }

    return ReadFileResult::READ_OK;
  };
}

auto const VersionIntHelper = cmJSONIntHelper<ReadFileResult>(
  ReadFileResult::READ_OK, ReadFileResult::INVALID_VERSION);

auto const VersionHelper = cmJSONRequiredHelper<int, ReadFileResult>(
  ReadFileResult::NO_VERSION, VersionIntHelper);

auto const RootVersionHelper =
  cmJSONObjectHelper<int, ReadFileResult>(ReadFileResult::READ_OK,
                                          ReadFileResult::INVALID_ROOT)
    .Bind("version"_s, VersionHelper, false);

auto const VariableStringHelper = cmJSONStringHelper<ReadFileResult>(
  ReadFileResult::READ_OK, ReadFileResult::INVALID_VARIABLE);

auto const VariableObjectHelper =
  cmJSONObjectHelper<CacheVariable, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_VARIABLE, false)
    .Bind("type"_s, &CacheVariable::Type, VariableStringHelper, false)
    .Bind("value"_s, &CacheVariable::Value, VariableStringHelper);

ReadFileResult VariableHelper(cm::optional<CacheVariable>& out,
                              const Json::Value* value)
{
  if (value->isString()) {
    out = CacheVariable{
      /*Type=*/"",
      /*Value=*/value->asString(),
    };
    return ReadFileResult::READ_OK;
  }
  if (value->isObject()) {
    out.emplace();
    return VariableObjectHelper(*out, value);
  }
  if (value->isNull()) {
    out = cm::nullopt;
    return ReadFileResult::READ_OK;
  }
  return ReadFileResult::INVALID_VARIABLE;
}

auto const VariablesHelper =
  cmJSONMapHelper<cm::optional<CacheVariable>, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET, VariableHelper);

auto const PresetStringHelper = cmJSONStringHelper<ReadFileResult>(
  ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET);

ReadFileResult EnvironmentHelper(cm::optional<std::string>& out,
                                 const Json::Value* value)
{
  if (!value || value->isNull()) {
    out = cm::nullopt;
    return ReadFileResult::READ_OK;
  }
  if (value->isString()) {
    out = value->asString();
    return ReadFileResult::READ_OK;
  }
  return ReadFileResult::INVALID_PRESET;
}

auto const EnvironmentMapHelper =
  cmJSONMapHelper<cm::optional<std::string>, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET,
    EnvironmentHelper);

auto const PresetVectorStringHelper =
  cmJSONVectorHelper<std::string, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET,
    PresetStringHelper);

ReadFileResult PresetInheritsHelper(std::vector<std::string>& out,
                                    const Json::Value* value)
{
  out.clear();
  if (!value) {
    return ReadFileResult::READ_OK;
  }

  if (value->isString()) {
    out.push_back(value->asString());
    return ReadFileResult::READ_OK;
  }

  return PresetVectorStringHelper(out, value);
}

auto const PresetBoolHelper = cmJSONBoolHelper<ReadFileResult>(
  ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET);

auto const PresetOptionalBoolHelper =
  cmJSONOptionalHelper<bool, ReadFileResult>(ReadFileResult::READ_OK,
                                             PresetBoolHelper);

auto const PresetWarningsHelper =
  cmJSONObjectHelper<UnexpandedPreset, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET, false)
    .Bind("dev"_s, &UnexpandedPreset::WarnDev, PresetOptionalBoolHelper, false)
    .Bind("deprecated"_s, &UnexpandedPreset::WarnDeprecated,
          PresetOptionalBoolHelper, false)
    .Bind("uninitialized"_s, &UnexpandedPreset::WarnUninitialized,
          PresetOptionalBoolHelper, false)
    .Bind("unusedCli"_s, &UnexpandedPreset::WarnUnusedCli,
          PresetOptionalBoolHelper, false)
    .Bind("systemVars"_s, &UnexpandedPreset::WarnSystemVars,
          PresetOptionalBoolHelper, false);

auto const PresetErrorsHelper =
  cmJSONObjectHelper<UnexpandedPreset, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET, false)
    .Bind("dev"_s, &UnexpandedPreset::ErrorDev, PresetOptionalBoolHelper,
          false)
    .Bind("deprecated"_s, &UnexpandedPreset::ErrorDeprecated,
          PresetOptionalBoolHelper, false);

auto const PresetDebugHelper =
  cmJSONObjectHelper<UnexpandedPreset, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET, false)
    .Bind("output"_s, &UnexpandedPreset::DebugOutput, PresetOptionalBoolHelper,
          false)
    .Bind("tryCompile"_s, &UnexpandedPreset::DebugTryCompile,
          PresetOptionalBoolHelper, false)
    .Bind("find"_s, &UnexpandedPreset::DebugFind, PresetOptionalBoolHelper,
          false);

ReadFileResult CMakeGeneratorConfigHelper(
  cm::optional<CMakeGeneratorConfig>& out, const Json::Value* value)
{
  if (!value) {
    out = cm::nullopt;
    return ReadFileResult::READ_OK;
  }

  if (!value->isString()) {
    return ReadFileResult::INVALID_PRESET;
  }

  if (value->asString() == "default") {
    out = CMakeGeneratorConfig::Default;
    return ReadFileResult::READ_OK;
  }

  if (value->asString() == "ignore") {
    out = CMakeGeneratorConfig::Ignore;
    return ReadFileResult::READ_OK;
  }

  return ReadFileResult::INVALID_PRESET;
}

auto const PresetHelper =
  cmJSONObjectHelper<UnexpandedPreset, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESET, false)
    .Bind("name"_s, &UnexpandedPreset::Name, PresetStringHelper)
    .Bind("inherits"_s, &UnexpandedPreset::Inherits, PresetInheritsHelper,
          false)
    .Bind("hidden"_s, &UnexpandedPreset::Hidden, PresetBoolHelper, false)
    .Bind<std::nullptr_t>("vendor"_s, nullptr,
                          VendorHelper(ReadFileResult::INVALID_PRESET), false)
    .Bind("displayName"_s, &UnexpandedPreset::DisplayName, PresetStringHelper,
          false)
    .Bind("description"_s, &UnexpandedPreset::Description, PresetStringHelper,
          false)
    .Bind("generator"_s, &UnexpandedPreset::Generator, PresetStringHelper,
          false)
    .Bind("architecture"_s, &UnexpandedPreset::Architecture,
          PresetStringHelper, false)
    .Bind("toolset"_s, &UnexpandedPreset::Toolset, PresetStringHelper, false)
    .Bind("cmakeGeneratorConfig"_s, &UnexpandedPreset::GeneratorConfig,
          CMakeGeneratorConfigHelper, false)
    .Bind("binaryDir"_s, &UnexpandedPreset::BinaryDir, PresetStringHelper,
          false)
    .Bind<std::string>("cmakeExecutable"_s, nullptr, PresetStringHelper, false)
    .Bind("cacheVariables"_s, &UnexpandedPreset::CacheVariables,
          VariablesHelper, false)
    .Bind("environment"_s, &UnexpandedPreset::Environment,
          EnvironmentMapHelper, false)
    .Bind("warnings"_s, PresetWarningsHelper, false)
    .Bind("errors"_s, PresetErrorsHelper, false)
    .Bind("debug"_s, PresetDebugHelper, false);

auto const PresetsHelper =
  cmJSONVectorHelper<UnexpandedPreset, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_PRESETS, PresetHelper);

auto const CMakeVersionUIntHelper = cmJSONUIntHelper<ReadFileResult>(
  ReadFileResult::READ_OK, ReadFileResult::INVALID_VERSION);

auto const CMakeVersionHelper =
  cmJSONObjectHelper<CMakeVersion, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_CMAKE_VERSION, false)
    .Bind("major"_s, &CMakeVersion::Major, CMakeVersionUIntHelper, false)
    .Bind("minor"_s, &CMakeVersion::Minor, CMakeVersionUIntHelper, false)
    .Bind("patch"_s, &CMakeVersion::Patch, CMakeVersionUIntHelper, false);

auto const RootPresetsHelper =
  cmJSONObjectHelper<RootPresets, ReadFileResult>(
    ReadFileResult::READ_OK, ReadFileResult::INVALID_ROOT, false)
    .Bind<int>("version"_s, nullptr, VersionHelper)
    .Bind("configurePresets"_s, &RootPresets::Presets, PresetsHelper, false)
    .Bind("cmakeMinimumRequired"_s, &RootPresets::CMakeMinimumRequired,
          CMakeVersionHelper, false)
    .Bind<std::nullptr_t>("vendor"_s, nullptr,
                          VendorHelper(ReadFileResult::INVALID_ROOT), false);

void InheritString(std::string& child, const std::string& parent)
{
  if (child.empty()) {
    child = parent;
  }
}

void InheritOptionalBool(cm::optional<bool>& child,
                         const cm::optional<bool>& parent)
{
  if (!child) {
    child = parent;
  }
}

/**
 * Check preset inheritance for cycles (using a DAG check algorithm) while
 * also bubbling up fields through the inheritance hierarchy, then verify
 * that each preset has the required fields, either directly or through
 * inheritance.
 */
ReadFileResult VisitPreset(std::map<std::string, UnexpandedPreset>& presets,
                           UnexpandedPreset& preset,
                           std::map<std::string, CycleStatus> cycleStatus)
{
  switch (cycleStatus[preset.Name]) {
    case CycleStatus::InProgress:
      return ReadFileResult::CYCLIC_PRESET_INHERITANCE;
    case CycleStatus::Verified:
      return ReadFileResult::READ_OK;
    default:
      break;
  }

  cycleStatus[preset.Name] = CycleStatus::InProgress;

  if (preset.CacheVariables.count("") != 0) {
    return ReadFileResult::INVALID_PRESET;
  }
  if (preset.Environment.count("") != 0) {
    return ReadFileResult::INVALID_PRESET;
  }

  for (auto const& i : preset.Inherits) {
    auto parent = presets.find(i);
    if (parent == presets.end()) {
      return ReadFileResult::INVALID_PRESET;
    }

    if (!preset.User && parent->second.User) {
      return ReadFileResult::USER_PRESET_INHERITANCE;
    }

    auto result = VisitPreset(presets, parent->second, cycleStatus);
    if (result != ReadFileResult::READ_OK) {
      return result;
    }

    InheritString(preset.Generator, parent->second.Generator);
    InheritString(preset.Architecture, parent->second.Architecture);
    InheritString(preset.Toolset, parent->second.Toolset);
    if (!preset.GeneratorConfig) {
      preset.GeneratorConfig = parent->second.GeneratorConfig;
    }
    InheritString(preset.BinaryDir, parent->second.BinaryDir);
    InheritOptionalBool(preset.WarnDev, parent->second.WarnDev);
    InheritOptionalBool(preset.ErrorDev, parent->second.ErrorDev);
    InheritOptionalBool(preset.WarnDeprecated, parent->second.WarnDeprecated);
    InheritOptionalBool(preset.ErrorDeprecated,
                        parent->second.ErrorDeprecated);
    InheritOptionalBool(preset.WarnUninitialized,
                        parent->second.WarnUninitialized);
    InheritOptionalBool(preset.WarnUnusedCli, parent->second.WarnUnusedCli);
    InheritOptionalBool(preset.WarnSystemVars, parent->second.WarnSystemVars);
    for (auto const& v : parent->second.CacheVariables) {
      preset.CacheVariables.insert(v);
    }
    for (auto const& v : parent->second.Environment) {
      preset.Environment.insert(v);
    }
  }

  if (!preset.Hidden) {
    if (preset.Generator.empty()) {
      return ReadFileResult::INVALID_PRESET;
    }
    if (preset.BinaryDir.empty()) {
      return ReadFileResult::INVALID_PRESET;
    }
    if (preset.WarnDev == false && preset.ErrorDev == true) {
      return ReadFileResult::INVALID_PRESET;
    }
    if (preset.WarnDeprecated == false && preset.ErrorDeprecated == true) {
      return ReadFileResult::INVALID_PRESET;
    }
  }

  cycleStatus[preset.Name] = CycleStatus::Verified;
  return ReadFileResult::READ_OK;
}

ReadFileResult ComputePresetInheritance(
  std::map<std::string, UnexpandedPreset>& presets)
{
  std::map<std::string, CycleStatus> cycleStatus;
  for (auto const& it : presets) {
    cycleStatus[it.first] = CycleStatus::Unvisited;
  }

  for (auto& it : presets) {
    auto result = VisitPreset(presets, it.second, cycleStatus);
    if (result != ReadFileResult::READ_OK) {
      return result;
    }
  }

  return ReadFileResult::READ_OK;
}

constexpr const char* ValidPrefixes[] = {
  "",
  "env",
  "penv",
  "vendor",
};

bool PrefixesValidMacroNamespace(const std::string& str)
{
  for (auto const& prefix : ValidPrefixes) {
    if (cmHasPrefix(prefix, str)) {
      return true;
    }
  }
  return false;
}

bool IsValidMacroNamespace(const std::string& str)
{
  for (auto const& prefix : ValidPrefixes) {
    if (str == prefix) {
      return true;
    }
  }
  return false;
}

bool VisitEnv(const cmCMakePresetsFile& file,
              cmCMakePresetsFile::ExpandedPreset& preset,
              std::map<std::string, CycleStatus>& envCycles,
              std::string& value, CycleStatus& status);
bool ExpandMacros(const cmCMakePresetsFile& file,
                  cmCMakePresetsFile::ExpandedPreset& preset,
                  std::map<std::string, CycleStatus>& envCycles,
                  std::string& out);
bool ExpandMacro(const cmCMakePresetsFile& file,
                 cmCMakePresetsFile::ExpandedPreset& preset,
                 std::map<std::string, CycleStatus>& envCycles,
                 std::string& out, const std::string& macroNamespace,
                 const std::string& macroName);

bool VisitEnv(const cmCMakePresetsFile& file,
              cmCMakePresetsFile::ExpandedPreset& preset,
              std::map<std::string, CycleStatus>& envCycles,
              std::string& value, CycleStatus& status)
{
  if (status == CycleStatus::Verified) {
    return true;
  }
  if (status == CycleStatus::InProgress) {
    return false;
  }

  status = CycleStatus::InProgress;
  if (!ExpandMacros(file, preset, envCycles, value)) {
    return false;
  }
  status = CycleStatus::Verified;
  return true;
}

bool ExpandMacros(const cmCMakePresetsFile& file,
                  cmCMakePresetsFile::ExpandedPreset& preset,
                  std::map<std::string, CycleStatus>& envCycles,
                  std::string& out)
{
  std::string result;
  std::string macroNamespace;
  std::string macroName;

  enum class State
  {
    Default,
    MacroNamespace,
    MacroName,
  } state = State::Default;

  for (auto c : out) {
    switch (state) {
      case State::Default:
        if (c == '$') {
          state = State::MacroNamespace;
        } else {
          result += c;
        }
        break;

      case State::MacroNamespace:
        if (c == '{') {
          if (IsValidMacroNamespace(macroNamespace)) {
            state = State::MacroName;
          } else {
            result += '$';
            result += macroNamespace;
            result += '{';
            macroNamespace.clear();
            state = State::Default;
          }
        } else {
          macroNamespace += c;
          if (!PrefixesValidMacroNamespace(macroNamespace)) {
            result += '$';
            result += macroNamespace;
            macroNamespace.clear();
            state = State::Default;
          }
        }
        break;

      case State::MacroName:
        if (c == '}') {
          if (!ExpandMacro(file, preset, envCycles, result, macroNamespace,
                           macroName)) {
            return false;
          }
          macroNamespace.clear();
          macroName.clear();
          state = State::Default;
        } else {
          macroName += c;
        }
        break;
    }
  }

  switch (state) {
    case State::Default:
      break;
    case State::MacroNamespace:
      result += '$';
      result += macroNamespace;
      break;
    case State::MacroName:
      return false;
  }

  out = std::move(result);
  return true;
}

bool ExpandMacro(const cmCMakePresetsFile& file,
                 cmCMakePresetsFile::ExpandedPreset& preset,
                 std::map<std::string, CycleStatus>& envCycles,
                 std::string& out, const std::string& macroNamespace,
                 const std::string& macroName)
{
  if (macroNamespace.empty()) {
    if (macroName == "sourceDir") {
      out += file.SourceDir;
      return true;
    }
    if (macroName == "sourceParentDir") {
      out += cmSystemTools::GetParentDirectory(file.SourceDir);
      return true;
    }
    if (macroName == "presetName") {
      out += preset.Name;
      return true;
    }
    if (macroName == "generator") {
      out += preset.Generator;
      return true;
    }
    if (macroName == "dollar") {
      out += '$';
      return true;
    }
  }

  if (macroNamespace == "env" && !macroName.empty()) {
    auto v = preset.Environment.find(macroName);
    if (v != preset.Environment.end() && v->second) {
      if (!VisitEnv(file, preset, envCycles, *v->second,
                    envCycles[macroName])) {
        return false;
      }
      out += *v->second;
      return true;
    }
  }

  if (macroNamespace == "env" || macroNamespace == "penv") {
    if (macroName.empty()) {
      return false;
    }
    const char* value = std::getenv(macroName.c_str());
    if (value) {
      out += value;
    }
    return true;
  }

  // "vendor" falls through to here
  return false;
}
}

std::string cmCMakePresetsFile::GetFilename(const std::string& sourceDir)
{
  return cmStrCat(sourceDir, "/CMakePresets.json");
}

std::string cmCMakePresetsFile::GetUserFilename(const std::string& sourceDir)
{
  return cmStrCat(sourceDir, "/CMakeUserPresets.json");
}

cmCMakePresetsFile::ReadFileResult cmCMakePresetsFile::ReadProjectPresets(
  const std::string& sourceDir, bool allowNoFiles)
{
  bool haveOneFile = false;
  this->SourceDir = sourceDir;
  this->Presets.clear();
  this->PresetOrder.clear();

  std::vector<std::string> presetOrder;
  std::map<std::string, UnexpandedPreset> presetMap;

  std::string filename = GetUserFilename(this->SourceDir);
  if (cmSystemTools::FileExists(filename)) {
    auto result = this->ReadJSONFile(filename, presetOrder, presetMap, true);
    if (result != ReadFileResult::READ_OK) {
      return result;
    }
    haveOneFile = true;
  }

  filename = GetFilename(this->SourceDir);
  if (cmSystemTools::FileExists(filename)) {
    auto result = this->ReadJSONFile(filename, presetOrder, presetMap, false);
    if (result != ReadFileResult::READ_OK) {
      return result;
    }
    haveOneFile = true;
  }

  if (!haveOneFile) {
    return allowNoFiles ? ReadFileResult::READ_OK
                        : ReadFileResult::FILE_NOT_FOUND;
  }

  auto result = ComputePresetInheritance(presetMap);
  if (result != ReadFileResult::READ_OK) {
    return result;
  }

  this->PresetOrder = std::move(presetOrder);
  this->Presets = std::move(presetMap);
  return ReadFileResult::READ_OK;
}

const char* cmCMakePresetsFile::ResultToString(ReadFileResult result)
{
  switch (result) {
    case ReadFileResult::READ_OK:
      return "OK";
    case ReadFileResult::FILE_NOT_FOUND:
      return "File not found";
    case ReadFileResult::JSON_PARSE_ERROR:
      return "JSON parse error";
    case ReadFileResult::INVALID_ROOT:
      return "Invalid root object";
    case ReadFileResult::NO_VERSION:
      return "No \"version\" field";
    case ReadFileResult::INVALID_VERSION:
      return "Invalid \"version\" field";
    case ReadFileResult::UNRECOGNIZED_VERSION:
      return "Unrecognized \"version\" field";
    case ReadFileResult::INVALID_CMAKE_VERSION:
      return "Invalid \"cmakeMinimumRequired\" field";
    case ReadFileResult::UNRECOGNIZED_CMAKE_VERSION:
      return "\"cmakeMinimumRequired\" version too new";
    case ReadFileResult::INVALID_PRESETS:
      return "Invalid \"configurePresets\" field";
    case ReadFileResult::INVALID_PRESET:
      return "Invalid preset";
    case ReadFileResult::INVALID_VARIABLE:
      return "Invalid CMake variable definition";
    case ReadFileResult::DUPLICATE_PRESETS:
      return "Duplicate presets";
    case ReadFileResult::CYCLIC_PRESET_INHERITANCE:
      return "Cyclic preset inheritance";
    case ReadFileResult::USER_PRESET_INHERITANCE:
      return "Project preset inherits from user preset";
    default:
      return "Unknown error";
  }
}

cm::optional<cmCMakePresetsFile::ExpandedPreset>
cmCMakePresetsFile::ExpandMacros(const UnexpandedPreset& preset) const
{
  ExpandedPreset retval{ preset };

  std::map<std::string, CycleStatus> envCycles;
  for (auto const& v : retval.Environment) {
    envCycles[v.first] = CycleStatus::Unvisited;
  }

  for (auto& v : retval.Environment) {
    if (v.second &&
        !VisitEnv(*this, retval, envCycles, *v.second, envCycles[v.first])) {
      return cm::nullopt;
    }
  }

  std::string binaryDir = preset.BinaryDir;
  if (!::ExpandMacros(*this, retval, envCycles, binaryDir)) {
    return cm::nullopt;
  }
  if (!cmSystemTools::FileIsFullPath(binaryDir)) {
    binaryDir = cmStrCat(this->SourceDir, '/', binaryDir);
  }
  retval.BinaryDir = cmSystemTools::CollapseFullPath(binaryDir);
  cmSystemTools::ConvertToUnixSlashes(retval.BinaryDir);

  for (auto& variable : retval.CacheVariables) {
    if (variable.second &&
        !::ExpandMacros(*this, retval, envCycles, variable.second->Value)) {
      return cm::nullopt;
    }
  }

  return cm::make_optional(retval);
}

cmCMakePresetsFile::ReadFileResult cmCMakePresetsFile::ReadJSONFile(
  const std::string& filename, std::vector<std::string>& presetOrder,
  std::map<std::string, UnexpandedPreset>& presetMap, bool user)
{
  cmsys::ifstream fin(filename.c_str());
  if (!fin) {
    return ReadFileResult::FILE_NOT_FOUND;
  }
  // If there's a BOM, toss it.
  cmsys::FStream::ReadBOM(fin);

  Json::Value root;
  Json::CharReaderBuilder builder;
  if (!Json::parseFromStream(builder, fin, &root, nullptr)) {
    return ReadFileResult::JSON_PARSE_ERROR;
  }

  int v = 0;
  auto result = RootVersionHelper(v, &root);
  if (result != ReadFileResult::READ_OK) {
    return result;
  }
  if (v < MIN_VERSION || v > MAX_VERSION) {
    return ReadFileResult::UNRECOGNIZED_VERSION;
  }

  RootPresets presets;
  if ((result = RootPresetsHelper(presets, &root)) !=
      ReadFileResult::READ_OK) {
    return result;
  }

  unsigned int currentMajor = cmVersion::GetMajorVersion();
  unsigned int currentMinor = cmVersion::GetMinorVersion();
  unsigned int currentPatch = cmVersion::GetPatchVersion();
  auto const& required = presets.CMakeMinimumRequired;
  if (required.Major > currentMajor ||
      (required.Major == currentMajor &&
       (required.Minor > currentMinor ||
        (required.Minor == currentMinor &&
         (required.Patch > currentPatch))))) {
    return ReadFileResult::UNRECOGNIZED_CMAKE_VERSION;
  }

  for (auto& preset : presets.Presets) {
    preset.User = user;
    if (preset.Name.empty()) {
      return ReadFileResult::INVALID_PRESET;
    }
    if (!presetMap.insert({ preset.Name, preset }).second) {
      return ReadFileResult::DUPLICATE_PRESETS;
    }
    presetOrder.push_back(preset.Name);
  }

  return ReadFileResult::READ_OK;
}
