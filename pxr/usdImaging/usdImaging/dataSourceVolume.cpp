//
// Copyright 2020 Pixar
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
#include "pxr/usdImaging/usdImaging/dataSourceVolume.h"

#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/volumeFieldBindingSchema.h"

PXR_NAMESPACE_OPEN_SCOPE

UsdImagingDataSourceVolumeFieldBindings
::UsdImagingDataSourceVolumeFieldBindings(
        UsdVolVolume usdVolume,
        const UsdImagingDataSourceStageGlobals &stageGlobals)
    : _usdVolume(usdVolume)
    , _stageGlobals(stageGlobals)
{
}

bool
UsdImagingDataSourceVolumeFieldBindings::Has(const TfToken &name)
{
    return _usdVolume.HasFieldRelationship(name);
}

TfTokenVector
UsdImagingDataSourceVolumeFieldBindings::GetNames()
{
    TRACE_FUNCTION();

    // XXX: This is more expensive than necessary, because we compute
    // relationship targets in addition to enumerating relationships.
    // Maybe ask for a UsdVolVolume.GetFieldRelationships call?
    const UsdVolVolume::FieldMap fields = _usdVolume.GetFieldPaths();
    TfTokenVector names;
    for (auto const& pair : fields) {
        names.push_back(pair.first);
    }
    return names;
}

HdDataSourceBaseHandle
UsdImagingDataSourceVolumeFieldBindings::Get(const TfToken &name)
{
    TRACE_FUNCTION();

    const SdfPath path = _usdVolume.GetFieldPath(name);
    if (path.IsEmpty()) {
        return nullptr;
    }

    return HdRetainedTypedSampledDataSource<SdfPath>::New(path);
}

// ----------------------------------------------------------------------------

UsdImagingDataSourceVolumePrim::UsdImagingDataSourceVolumePrim(
        const SdfPath &sceneIndexPath,
        UsdPrim usdPrim,
        const UsdImagingDataSourceStageGlobals &stageGlobals)
    : UsdImagingDataSourceGprim(sceneIndexPath, usdPrim, stageGlobals)
{
}

bool 
UsdImagingDataSourceVolumePrim::Has(
    const TfToken &name)
{
    if (name == HdVolumeFieldBindingSchemaTokens->volumeFieldBinding) {
        return true;
    }

    return UsdImagingDataSourceGprim::Has(name);
}

TfTokenVector 
UsdImagingDataSourceVolumePrim::GetNames()
{
    TfTokenVector result = UsdImagingDataSourceGprim::GetNames();
    result.push_back(HdVolumeFieldBindingSchemaTokens->volumeFieldBinding);

    return result;
}

HdDataSourceBaseHandle 
UsdImagingDataSourceVolumePrim::Get(const TfToken &name)
{
    if (name == HdVolumeFieldBindingSchemaTokens->volumeFieldBinding) {
        return UsdImagingDataSourceVolumeFieldBindings::New(
            UsdVolVolume(_GetUsdPrim()), _GetStageGlobals());
    } else {
        return UsdImagingDataSourceGprim::Get(name);
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
