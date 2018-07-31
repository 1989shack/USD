//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
///
/// \file usdUtils/dependencies.cpp
#include "pxr/pxr.h"
#include "pxr/usd/usdUtils/dependencies.h"

#include "pxr/usd/ar/packageUtils.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/layerUtils.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/reference.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/sdf/variantSetSpec.h"
#include "pxr/usd/sdf/variantSpec.h"
#include "pxr/usd/usd/clipsAPI.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/tokens.h"
#include "pxr/usd/usd/usdFileFormat.h"
#include "pxr/usd/usd/zipFile.h"

#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/tf/fileUtils.h"
#include "pxr/base/tf/pathUtils.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/trace/trace.h"

#include <stack>
#include <vector>

#include <boost/get_pointer.hpp>

PXR_NAMESPACE_OPEN_SCOPE

using std::vector;
using std::string;

namespace {

// Enum class representing the type of dependency.
enum class _DepType {
    Reference,
    Sublayer,
    Payload
};

// Enum class representing the type of an asset path.
enum class _PathType {
    RelativePath,
    SearchPath,
    AbsolutePath
};

class _FileAnalyzer {
public:
    // Takes a given asset path and the layer it was found in, and returns the
    // corresponding remapped path.
    // The layer is used to resolve the asset path in cases where the given 
    // asset path is a search path or a relative path. 
    using RemapAssetPathFunc = std::function<std::string 
            (const std::string &, const SdfLayerRefPtr &)>;

    // Takes the asset path and the type of dependency it is and does some 
    // arbitrary processing (like enumerating dependencies).
    using ProcessAssetPathFunc = std::function<void (const std::string &, 
                                                     const _DepType &)>;

    // Opens the file at \p resolvedFilePath and analyzes its external 
    // dependencies.
    // 
    // For each dependency that is detected, the provided (optional) callback 
    // functions are invoked. 
    // 
    // \p processPathFunc is invoked first with the raw (un-remapped) path. 
    // Then \p remapPathFunc is invoked. 
    _FileAnalyzer(const std::string &resolvedFilePath,
                  const RemapAssetPathFunc &remapPathFunc={},
                  const ProcessAssetPathFunc &processPathFunc={}) : 
        _filePath(resolvedFilePath),
        _remapPathFunc(remapPathFunc),
        _processPathFunc(processPathFunc)
    {
        _AnalyzeDependencies();
    }

    // Returns the path to the file on disk that is being analyzed.
    const std::string &GetFilePath() const {
        return _filePath;
    }

    // Returns the SdfLayer associated with the file being analyzed.
    const SdfLayerRefPtr &GetLayer() const {
        return _layer;
    }

private:
    // Open the layer, updates references to point to relative or search paths
    // and accumulates all references.
    void _AnalyzeDependencies();

    // This adds the given raw reference path to the list of all referenced 
    // paths. It also returns the remapped reference path, so client code
    // can update the source reference to point to the remapped path.
    std::string _ProcessDependency(const std::string &rawRefPath,
                                   const _DepType &depType);

    // Processes any sublayers in the SdfLayer associated with the file.
    void _ProcessSublayers();

    // Processes the payload on the given primSpec.
    void _ProcessPayload(const SdfPrimSpecHandle &primSpec);

    // Processes prim metadata.
    void _ProcessMetadata(const SdfPrimSpecHandle &primSpec);

    // Processes metadata on properties.
    void _ProcessProperties(const SdfPrimSpecHandle &primSpec);

    // Processes all references on the given primSpec.
    void _ProcessReferences(const SdfPrimSpecHandle &primSpec);

    // Returns the given VtValue with any asset paths remapped to point to 
    // destination-relative path.
    VtValue _UpdateAssetValue(const VtValue &val);

    // Callback function that's passed into SdfReferencesProxy::ModifyItemEdits()
    // to update all references.
    boost::optional<SdfReference> _RemapSdfReference(
            const SdfReference &reference);

    // Resolved path to the file.
    std::string _filePath;

    // SdfLayer corresponding to the file. This will be null for non-layer 
    // files.
    SdfLayerRefPtr _layer;

    // Remap and process path callback functions.
    RemapAssetPathFunc _remapPathFunc;
    ProcessAssetPathFunc _processPathFunc;
};

std::string 
_FileAnalyzer::_ProcessDependency(const std::string &rawRefPath,
                                  const _DepType &depType)
{
    if (_processPathFunc) {
        _processPathFunc(rawRefPath, depType);
    }

    if (_remapPathFunc) {
        return _remapPathFunc(rawRefPath, GetLayer());
    } else {
        return rawRefPath;
    }
}

VtValue 
_FileAnalyzer::_UpdateAssetValue(const VtValue &val) 
{
    if (val.IsHolding<SdfAssetPath>()) {
        auto assetPath = val.UncheckedGet<SdfAssetPath>();
        std::string rawAssetPath = assetPath.GetAssetPath();
        if (!rawAssetPath.empty()) {
            return VtValue(SdfAssetPath(
                    _ProcessDependency(rawAssetPath, _DepType::Reference)));
        }
    } else if (val.IsHolding<VtArray<SdfAssetPath>>()) {
        VtArray<SdfAssetPath> updatedVal;
        for (const SdfAssetPath& assetPath :
            val.UncheckedGet< VtArray<SdfAssetPath> >()) {                
            std::string rawAssetPath = assetPath.GetAssetPath();
            if (!rawAssetPath.empty()) {
                updatedVal.push_back(SdfAssetPath(
                        _ProcessDependency(rawAssetPath, _DepType::Reference)));
            } else {
                // Retain empty paths in the array.
                updatedVal.push_back(assetPath);
            }
        }
        return VtValue(updatedVal);
    }
    else if (val.IsHolding<VtDictionary>()) {
        VtDictionary updatedVal;
        for (const auto& p : val.UncheckedGet<VtDictionary>()) {
            updatedVal[p.first] = _UpdateAssetValue(p.second);
        }
        return VtValue(updatedVal);
    }

    return val;
}

void
_FileAnalyzer::_ProcessSublayers()
{
    const std::vector<std::string> subLayerPaths = _layer->GetSubLayerPaths();

    if (_remapPathFunc) {
        std::vector<std::string> newSubLayerPaths;
        newSubLayerPaths.reserve(subLayerPaths.size());
        for (auto &subLayer: subLayerPaths) {
            newSubLayerPaths.push_back(
                _ProcessDependency(subLayer, _DepType::Sublayer));
        }
        _layer->SetSubLayerPaths(newSubLayerPaths);
    } else {
        for (auto &subLayer: subLayerPaths) {
            _ProcessDependency(subLayer, _DepType::Sublayer);
        }
    }
}

void
_FileAnalyzer::_ProcessPayload(const SdfPrimSpecHandle &primSpec)
{
    if (primSpec->HasPayload()) {
        SdfPayload payload = primSpec->GetPayload();
        auto &payloadPath = payload.GetAssetPath();
        auto remappedPayloadPath = _ProcessDependency(payloadPath, 
                _DepType::Payload);

        if (_remapPathFunc) {
            payload.SetAssetPath(remappedPayloadPath);
            primSpec->SetPayload(payload);
        }
    }
}

void
_FileAnalyzer::_ProcessProperties(const SdfPrimSpecHandle &primSpec)
{
    // XXX:2016-04-14 Note that we use the field access API
    // here rather than calling GetAttributes, as creating specs for
    // large numbers of attributes, most of which are *not* asset
    // path-valued and therefore not useful here, is expensive.
    //
    const VtValue propertyNames =
        primSpec->GetField(SdfChildrenKeys->PropertyChildren);

    if (propertyNames.IsHolding<vector<TfToken>>()) {
        for (const auto& name :
                propertyNames.UncheckedGet<vector<TfToken>>()) {
            // For every property
            // Build an SdfPath to the property
            const SdfPath path = primSpec->GetPath().AppendProperty(name);

            // Check property metadata
            for (const TfToken& infoKey : _layer->ListFields(path)) {
                if (infoKey != SdfFieldKeys->Default &&
                    infoKey != SdfFieldKeys->TimeSamples) {
                        
                    VtValue value = _layer->GetField(path, infoKey);
                    VtValue updatedValue = _UpdateAssetValue(value);
                    if (_remapPathFunc && value != updatedValue) {
                        _layer->SetField(path, infoKey, updatedValue);
                    }
                }
            }

            // Check property existence
            const VtValue vtTypeName =
                _layer->GetField(path, SdfFieldKeys->TypeName);
            if (!vtTypeName.IsHolding<TfToken>())
                continue;

            const TfToken typeName =
                vtTypeName.UncheckedGet<TfToken>();
            if (typeName == SdfValueTypeNames->Asset ||
                typeName == SdfValueTypeNames->AssetArray) {

                // Check default value
                VtValue defValue = _layer->GetField(path, 
                        SdfFieldKeys->Default);
                VtValue updatedDefValue = _UpdateAssetValue(defValue);
                if (_remapPathFunc && defValue != updatedDefValue) {
                    _layer->SetField(path, SdfFieldKeys->Default, 
                            updatedDefValue);
                }

                // Check timeSample values
                for (double t : _layer->ListTimeSamplesForPath(path)) {
                    VtValue timeSampleVal;
                    if (_layer->QueryTimeSample(path,
                        t, &timeSampleVal)) {

                        VtValue updatedTimeSampleVal = 
                            _UpdateAssetValue(timeSampleVal);
                        if (_remapPathFunc && 
                            timeSampleVal != updatedTimeSampleVal) {
                            _layer->SetTimeSample(path, t, 
                                    updatedTimeSampleVal);
                        }
                    }
                }
            }
        }
    }
}

void
_FileAnalyzer::_ProcessMetadata(const SdfPrimSpecHandle &primSpec)
{
    for (const TfToken& infoKey : primSpec->GetMetaDataInfoKeys()) {
        VtValue value = primSpec->GetInfo(infoKey);
        VtValue updatedValue = _UpdateAssetValue(value);
        if (_remapPathFunc && value != updatedValue) {
            primSpec->SetInfo(infoKey, updatedValue);
        }
    }

    // Process clips["templateAssetPath"], which is a string value 
    // containing one or more #'s. See 
    // UsdClipsAPI::GetClipTemplateAssetPath for details. 
    VtValue clipsValue = primSpec->GetInfo(UsdTokens->clips);
    if (!clipsValue.IsEmpty() && clipsValue.IsHolding<VtDictionary>()) {
        VtDictionary clipsDict = 
                clipsValue.UncheckedGet<VtDictionary>();
        for (auto &clipSetNameAndDict : clipsDict) {
            if (clipSetNameAndDict.second.IsHolding<VtDictionary>()) {
                VtDictionary clipDict = 
                    clipSetNameAndDict.second.UncheckedGet<VtDictionary>();

                if (VtDictionaryIsHolding<std::string>(clipDict, 
                        UsdClipsAPIInfoKeys->templateAssetPath
                            .GetString())) {
                    const std::string &templateAssetPath = 
                            VtDictionaryGet<std::string>(clipDict, 
                                UsdClipsAPIInfoKeys->templateAssetPath
                                    .GetString());

                    if (templateAssetPath.empty()) {
                        continue;
                    }

                    // Remap templateAssetPath if there's a remap function and 
                    // update the clip dictionary.
                    // This retains the #s in the templateAssetPath?
                    if (_remapPathFunc) {
                        clipDict[UsdClipsAPIInfoKeys->templateAssetPath] = 
                            VtValue(_remapPathFunc(templateAssetPath, 
                                                   GetLayer()));
                        clipsDict[clipSetNameAndDict.first] = VtValue(clipDict);
                    }

                    // Compute the resolved location of the clips 
                    // directory, so we can do a TfGlob for the pattern.
                    // This contains a '/' in the end.
                    const std::string clipsDir = TfGetPathName(
                            templateAssetPath);
                    // Resolve clipsDir relative to this layer. 
                    const std::string clipsDirAssetPath = 
                        SdfComputeAssetPathRelativeToLayer(_layer, clipsDir);

                    std::string resolvedClipsDir = 
                        ArGetResolver().Resolve(clipsDirAssetPath);
                    if (resolvedClipsDir.empty()) {
                        TF_WARN("Failed to resolve template clips directory"
                            " @%s@ with computed asset path @%s@ in layer "
                            "@%s@.", clipsDir.c_str(), 
                            clipsDirAssetPath.c_str(), 
                            GetFilePath().c_str());
                        continue;
                    }

                    if (!TfIsDir(resolvedClipsDir)) {
                        TF_WARN("Resolved path to clips directory '%s' is not "
                            "a valid directory!", resolvedClipsDir.c_str());
                        continue;
                    }

                    std::string clipsBaseName = TfGetBaseName(
                            templateAssetPath);
                    std::string globPattern = TfStringCatPaths(
                            resolvedClipsDir, 
                            TfStringReplace(clipsBaseName, "#", "*"));
                    const std::vector<std::string> clipAssetRefs = 
                        TfGlob(globPattern);
                    for (auto &clipAsset : clipAssetRefs) {
                        // Reconstruct the raw, unresolved clip reference, for 
                        // which the dependency must be processed.
                        // 
                        // clipsDir contains a '/' in the end, but 
                        // resolvedClipsDir does not. Hence, add a '/' to 
                        // resolvedClipsDir before doing the replace.
                        std::string rawClipRef = TfStringReplace(
                                clipAsset, resolvedClipsDir + '/', clipsDir);
                        _ProcessDependency(rawClipRef, _DepType::Reference);
                    }
                }
            }
        }

        if (_remapPathFunc) {
            primSpec->SetInfo(UsdTokens->clips, VtValue(clipsDict));
        }
    }
}

boost::optional<SdfReference>
_FileAnalyzer::_RemapSdfReference(const SdfReference &reference) 
{
    // If this is a local (or self) reference, there's no asset path to update.
    if (reference.GetAssetPath().empty()) {
        return reference;
    }

    std::string remappedRefPath = _ProcessDependency(reference.GetAssetPath(),
            _DepType::Reference);
    // If the path was not remapped to a different path, then return the 
    // incoming reference unmodifed.
    if (remappedRefPath == reference.GetAssetPath())
        return reference;

    // The reference path was remapped, hence construct a new SdfReference
    // object with the remapped path.
    SdfReference remappedRef = reference;
    remappedRef.SetAssetPath(remappedRefPath);
    return remappedRef;
}

void
_FileAnalyzer::_ProcessReferences(const SdfPrimSpecHandle &primSpec)
{
    SdfReferencesProxy refList = primSpec->GetReferenceList();
    refList.ModifyItemEdits(std::bind(&_FileAnalyzer::_RemapSdfReference, 
            this, std::placeholders::_1));
}

void
_FileAnalyzer::_AnalyzeDependencies()
{
    // If this file can be opened on a USD stage or referenced into a USD 
    // stage via composition, then analyze the file, collect & update all 
    // references. If not, return early.
    if (!UsdStage::IsSupportedFile(_filePath)) {
        return;
    }

    TRACE_FUNCTION();

    _layer = SdfLayer::FindOrOpen(_filePath);
    if (!_layer) {
        TF_WARN("Unable to open layer at path @%s@.", _filePath.c_str());
        return;
    }

    _ProcessSublayers();

    std::stack<SdfPrimSpecHandle> dfs;
    dfs.push(_layer->GetPseudoRoot());

    while (!dfs.empty()) {
        SdfPrimSpecHandle curr = dfs.top();
        dfs.pop();

        if (curr != _layer->GetPseudoRoot()) {
            _ProcessPayload(curr);    
            _ProcessProperties(curr);
            _ProcessMetadata(curr);
            _ProcessReferences(curr);
        }

        // variants "children"
        for (const SdfVariantSetsProxy::value_type& p :
            curr->GetVariantSets()) {
            for (const SdfVariantSpecHandle& variantSpec :
                p.second->GetVariantList()) {
                dfs.push(variantSpec->GetPrimSpec());
            }
        }

        // children
        for (const SdfPrimSpecHandle& child : curr->GetNameChildren()) {
            dfs.push(child);
        }
    }
}

class _AssetLocalizer {
public:
    using LayerAndDestPath = std::pair<SdfLayerRefPtr, std::string>;
    using SrcPathAndDestPath = std::pair<std::string, std::string>;
    using DestFilePathAndAnalyzer = std::pair<std::string, _FileAnalyzer>;
    using LayerDependenciesMap = std::unordered_map<SdfLayerRefPtr, 
            std::vector<std::string>, TfHash>;

    _AssetLocalizer(const SdfAssetPath &assetPath, const std::string &destDir) 
    {
        auto &layerDependenciesMap = _layerDependenciesMap;
        const auto remapAssetPathFunc = [&layerDependenciesMap](
            const std::string &ap, const SdfLayerRefPtr &layer) {
            layerDependenciesMap[layer].push_back(ap);
            return _RemapAssetPath(ap, layer, nullptr);
        };

        auto &resolver = ArGetResolver();

        std::unordered_set<std::string> seenFiles;
        std::stack<DestFilePathAndAnalyzer> filesToLocalize;
        {
            std::string filePath = resolver.Resolve(
                    assetPath.GetAssetPath());

            if (!filePath.empty()) {
                // Ensure that the resolved path resolves to physical location
                // on disk.
                ArGetResolver().FetchToLocalResolvedPath(
                        assetPath.GetAssetPath(), filePath);

                seenFiles.insert(filePath);

                std::string destFilePath = TfStringCatPaths(destDir, 
                        TfGetBaseName(filePath));
                filesToLocalize.emplace(destFilePath, 
                        _FileAnalyzer(filePath, remapAssetPathFunc));
            }
        }

        while (!filesToLocalize.empty()) {
            // Copying data here since we're about to pop.
            const DestFilePathAndAnalyzer destFilePathAndAnalyzer = 
                filesToLocalize.top();
            filesToLocalize.pop();

            auto &destFilePath = destFilePathAndAnalyzer.first;
            auto &fileAnalyzer = destFilePathAndAnalyzer.second;

            if (!fileAnalyzer.GetLayer()) {
                _fileCopyMap.emplace_back(fileAnalyzer.GetFilePath(),
                                          destFilePath);
                continue;
            }

            _layerExportMap.emplace_back(fileAnalyzer.GetLayer(), 
                                         destFilePath);

            const auto &layerDepIt = layerDependenciesMap.find(
                    fileAnalyzer.GetLayer());

            if (layerDepIt == _layerDependenciesMap.end()) {
                // The layer has no external dependencies.
                continue;
            }

            for (std::string ref : layerDepIt->second) {
                // If this is a package-relative path, then simply copy the 
                // package over. 
                // Note: recursive search for dependencies ends here. 
                // This is because we don't want to be modifying packaged 
                // assets during asset isolation or archival. 
                // XXX: We may want to reconsider this approach in the future.
                if (ArIsPackageRelativePath(ref)) {
                    ref = ArSplitPackageRelativePathOuter(ref).first;
                }

                const std::string refAssetPath = 
                        SdfComputeAssetPathRelativeToLayer(
                            fileAnalyzer.GetLayer(), ref);

                std::string resolvedRefFilePath = resolver.Resolve(refAssetPath);

                if (resolvedRefFilePath.empty()) {
                    TF_WARN("Failed to resolve reference @%s@ with computed "
                            "asset path @%s@ found in layer @%s@.", 
                            ref.c_str(),
                            refAssetPath.c_str(), 
                            fileAnalyzer.GetFilePath().c_str());
                    continue;
                } 

                // Ensure that the resolved path can be fetched to a physical 
                // location on disk.
                ArGetResolver().FetchToLocalResolvedPath(refAssetPath, 
                                                         resolvedRefFilePath);

                // Given the way our remap function (_RemapAssetPath) works, we 
                // should only have to copy every resolved file once during
                // localization.
                if (!seenFiles.insert(resolvedRefFilePath).second) {
                    continue;
                }

                // XXX: We don't localize directory references. Should we copy 
                // the entire directory over?
                if (TfIsDir(resolvedRefFilePath)) {
                    continue;
                }

                _PathType pathType;
                std::string remappedRef = _RemapAssetPath(ref, 
                    fileAnalyzer.GetLayer(), &pathType);

                // If it's a relative path, construct the full path relative to
                // the final (destination) location of the reference-containing 
                // file.
                const std::string destDirForRef = 
                        (pathType == _PathType::RelativePath) ? 
                        TfGetPathName(destFilePath) : 
                        destDir; 
                const std::string destFilePathForRef = TfStringCatPaths(
                        destDirForRef, remappedRef);

                filesToLocalize.emplace(destFilePathForRef, 
                        _FileAnalyzer(resolvedRefFilePath, 
                                        remapAssetPathFunc));
            }
        }
    }

    // Get the list of layers to be localized along with their corresponding 
    // destination paths.
    const std::vector<LayerAndDestPath> &GetLayerExportMap() const {
        return _layerExportMap;
    }

    // Get the list of source files to be copied along with their corresponding 
    // destination paths.
    const std::vector<SrcPathAndDestPath> &GetFileCopyMap() const {
        return _fileCopyMap;
    }

private:
    // This will contain a mapping of SdfLayerRefPtr's mapped to their 
    // desination path inside the destination directory.
    std::vector<LayerAndDestPath> _layerExportMap;

    // This will contain a mapping of source file path to the corresponding 
    // desination file path.
    std::vector<SrcPathAndDestPath> _fileCopyMap;

    // A map of layers and their corresponding vector of raw external reference
    // paths.
    LayerDependenciesMap _layerDependenciesMap;

    // Remaps a given asset path to be relative to the layer containing it,
    // for the purpose of localization.
    static std::string _RemapAssetPath(const std::string &refPath, 
                                       const SdfLayerRefPtr &layer,
                                       _PathType *pathType);
};

std::string 
_AssetLocalizer::_RemapAssetPath(const std::string &refPath, 
                                 const SdfLayerRefPtr &layer,
                                 _PathType *pathType)
{
    auto &resolver = ArGetResolver();

    bool isSearchPath = resolver.IsSearchPath(refPath);

    // Return relative paths unmodified.
    if (!isSearchPath && resolver.IsRelativePath(refPath)) {
        if (pathType) {
            *pathType = _PathType::RelativePath;
        }
        return refPath;
    }

    std::string result = refPath;
    if (isSearchPath) {
        // If it is a search-path, resolve it to an absolute path on disk.
        if (pathType) {
            *pathType = _PathType::SearchPath;
        }

        // Absolutize the search path, to avoid collisions resulting from the 
        // same search path resolving to different paths in different resolver
        // contexts.
        const std::string refAssetPath = 
                SdfComputeAssetPathRelativeToLayer(layer, refPath);
        const std::string refFilePath = resolver.Resolve(refAssetPath);

        // Ensure that the resolved path can be fetched to a physical 
        // location on disk.
        if (!refFilePath.empty()) {
            ArGetResolver().FetchToLocalResolvedPath(refAssetPath, 
                                                     refFilePath);
            result = refFilePath;
        } else {
            // Failed to resolve asset path, hence retain the reference as is.
            result = refAssetPath;
        }
    } else if (pathType) {
        *pathType = _PathType::AbsolutePath;
    }

    // Result is now an absolute or a repository path. Simply strip off the 
    // leading slashes to make it relative.
    result = resolver.ComputeNormalizedPath(result);

    // Strip off any drive letters.
    if (result.size() >= 2 && result[1] == ':') {
        result.erase(0, 2);
    }

    // Strip off any initial slashes.
    return TfStringTrimLeft(result, "/");
}

// Returns a relative path for fullDestPath, relative to the given destination 
// directory (destDir).
static 
std::string 
_GetDestRelativePath(const std::string &fullDestPath, 
                     const std::string &destDir)
{
    std::string destPath = fullDestPath;
    // fullDestPath won't start with destDir if destDir is a relative path, 
    // relative to CWD.
    if (TfStringStartsWith(destPath, destDir)) {
        destPath = destPath.substr(destDir.length());
    }
    return destPath;
}

} // end of anonymous namespace


// XXX: don't even know if it's important to distinguish where
// these asset paths are coming from..  if it's not important, maybe this
// should just go into Sdf's _GatherPrimAssetReferences?  if it is important,
// we could also have another function that takes 3 vectors.
void
UsdUtilsExtractExternalReferences(
    const std::string& filePath,
    std::vector<std::string>* subLayers,
    std::vector<std::string>* references,
    std::vector<std::string>* payloads)
{
    TRACE_FUNCTION();

    // We only care about knowing what the dependencies are. Hence, set 
    // remapPathFunc to empty.
    _FileAnalyzer(filePath, /*remapPathFunc*/ {}, 
        [&subLayers, &references, &payloads](const std::string &assetPath,
                                          const _DepType &depType) {
            if (depType == _DepType::Reference) {
                references->push_back(assetPath);
            } else if (depType == _DepType::Sublayer) {
                subLayers->push_back(assetPath);
            } else if (depType == _DepType::Payload) {
                payloads->push_back(assetPath);
            }
        });

    // Sort and remove duplicates
    std::sort(references->begin(), references->end());
    references->erase(std::unique(references->begin(), references->end()),
        references->end());
    std::sort(payloads->begin(), payloads->end());
    payloads->erase(std::unique(payloads->begin(), payloads->end()),
        payloads->end());
}


bool
UsdUtilsCreateNewUsdzPackage(const SdfAssetPath &assetPath,
                             const std::string &usdzFilePath,
                             const std::string &firstLayerName)
{
    std::string destDir = TfGetPathName(usdzFilePath);

    _AssetLocalizer localizer(assetPath, destDir);

    auto &layerExportMap = localizer.GetLayerExportMap();
    auto &fileCopyMap = localizer.GetFileCopyMap();

    if (layerExportMap.empty() && fileCopyMap.empty()) {
        return false;
    }

    // Set of all the packaged files.
    std::unordered_set<std::string> packagedFiles;

    const std::string tmpDirPath = ArchGetTmpDir();

    UsdZipFileWriter writer = UsdZipFileWriter::CreateNew(usdzFilePath);

    bool firstLayer = true;
    bool success = true;
    for (auto &layerAndDestPath : layerExportMap) {
        const auto &layer = layerAndDestPath.first;
        std::string destPath = _GetDestRelativePath(
                layerAndDestPath.second, destDir);

        // Change the first layer's name if requested.
        if (firstLayer && !firstLayerName.empty()) {
            const std::string pathName = TfGetPathName(destPath);
            destPath = TfStringCatPaths(pathName, firstLayerName);
            firstLayer = false;
        }

        if (!packagedFiles.insert(destPath).second) {
            TF_WARN("A file already exists at path \"%s\" in the package. "
                "Skipping export of layer @%s@.", destPath.c_str(), 
                layer->GetIdentifier().c_str());
            continue;
        }

        // If the layer hasn't been modified from its persistent representation, 
        // then simply copy it over from its real-path (i.e. location on disk).
        // This preserves any existing comments in the file (Which are lost 
        // when exporting).
        if (!layer->IsDirty()) {
            std::string inArchivePath = writer.AddFile(layer->GetRealPath(), 
                    destPath);
            if (inArchivePath.empty()) {
                success = false;
            }
        } else {
            // If the layer has been modified, then we need to export it to 
            // a temporary file before adding it to the package.
            std::string layerExtension = ArGetResolver().GetExtension(destPath);
            SdfFileFormat::FileFormatArguments args;

            const SdfFileFormatConstPtr fileFormat = 
                    SdfFileFormat::FindByExtension(
                        SdfFileFormat::GetFileExtension(destPath));

            if (TfDynamic_cast<UsdUsdFileFormatConstPtr>(fileFormat)) {
                args[UsdUsdFileFormatTokens->FormatArg] = 
                        UsdUsdFileFormat::GetUnderlyingFormatForLayer(
                            boost::get_pointer(layer));
            }
            
            std::string tmpLayerExportPath = TfStringCatPaths(tmpDirPath, 
                    TfGetBaseName(destPath));
            layer->Export(tmpLayerExportPath, /*comment*/ "", args);

            std::string inArchivePath = writer.AddFile(tmpLayerExportPath, 
                    destPath);

            if (inArchivePath.empty()) {
                // XXX: Should we discard the usdz file and return early here?
                TF_WARN("Failed to add temporary layer at '%s' to the package "
                    "at path '%s'.", tmpLayerExportPath.c_str(), 
                    usdzFilePath.c_str());
                success = false;
            } else {
                // The file has been added to the package successfully. We can 
                // delete it now.
                TfDeleteFile(tmpLayerExportPath);
            }
        }
    }

    for (auto &fileSrcAndDestPath : fileCopyMap) {
        const std::string &srcPath = fileSrcAndDestPath.first;
        const std::string destPath = _GetDestRelativePath(
                fileSrcAndDestPath.second, destDir);

        if (!packagedFiles.insert(destPath).second) {
            TF_WARN("A file already exists at path \"%s\" in the package. "
                "Skipping copy of file \"%s\".", destPath.c_str(), 
                srcPath.c_str());
            continue;
        }

        std::string inArchivePath = writer.AddFile(srcPath, destPath);
        if (inArchivePath.empty()) {
            // XXX: Should we discard the usdz file and return early here?
            TF_WARN("Failed to add file '%s' to the package at path '%s'.",
                    srcPath.c_str(), usdzFilePath.c_str());
            success = false;
        }
    }

    return writer.Save() && success;
}

PXR_NAMESPACE_CLOSE_SCOPE
