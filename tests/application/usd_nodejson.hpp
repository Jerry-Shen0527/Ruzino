#pragma once

#include <pxr/usd/sdf/path.h>

#include <fstream>
#include <string>

#include "nodes/ui/imgui.hpp"
#include "stage/stage.hpp"

struct UsdBasedNodeStorage : public Ruzino::NodeSystemStorage {
    UsdBasedNodeStorage(Ruzino::Stage* stage, const pxr::SdfPath& path)
        : stage_(stage),
          path_(path)
    {
    }
    void save(const std::string& data) override;
    std::string load() override;
    pxr::SdfPath path_;
    Ruzino::Stage* stage_;
};

struct UsdBasedNodeWidgetSettings : public Ruzino::NodeWidgetSettings {
    pxr::SdfPath json_path;
    Ruzino::Stage* stage;

    std::string WidgetName() const override;

    std::unique_ptr<Ruzino::NodeSystemStorage> create_storage() const override;
};

inline void UsdBasedNodeStorage::save(const std::string& data)
{
    stage_->save_string_to_usd(path_, data);
}

inline std::string UsdBasedNodeStorage::load()
{
    return stage_->load_string_from_usd(path_);
}

inline std::string UsdBasedNodeWidgetSettings::WidgetName() const
{
    return json_path.GetString();
}

inline std::unique_ptr<Ruzino::NodeSystemStorage>
UsdBasedNodeWidgetSettings::create_storage() const
{
    return std::make_unique<UsdBasedNodeStorage>(stage, json_path);
}
