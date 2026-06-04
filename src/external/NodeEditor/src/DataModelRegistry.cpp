#include "DataModelRegistry.hpp"

#include <QtCore/QFile>
#include <QtWidgets/QMessageBox>

using QtNodes::DataModelRegistry;
using QtNodes::NodeDataModel;
using QtNodes::NodeDataType;
using QtNodes::TypeConverter;

std::unique_ptr<NodeDataModel>
DataModelRegistry::
create(QString const &modelName)
{
  auto it = _registeredItemCreators.find(modelName);

  if (it != _registeredItemCreators.end())
  {
    return it->second();
  }

  RegisteredModelCreatorsMap::value_type const* deferredEntry = nullptr;
  RegisteredModelsCategoryMap::value_type const* deferredCategory = nullptr;
  std::unique_ptr<NodeDataModel> resolvedModel;

  for (auto const& entry : _registeredItemCreators)
  {
    if (!entry.first.startsWith(QStringLiteral("__lazy__::")))
    {
      continue;
    }

    auto candidate = entry.second();
    if (!candidate || candidate->name() != modelName)
    {
      continue;
    }

    deferredEntry = &entry;

    auto categoryIt = _registeredModelsCategory.find(entry.first);
    if (categoryIt != _registeredModelsCategory.end())
    {
      deferredCategory = &(*categoryIt);
    }

    resolvedModel = std::move(candidate);
    break;
  }

  if (deferredEntry)
  {
    _registeredItemCreators[modelName] = deferredEntry->second;
    if (deferredCategory)
    {
      _registeredModelsCategory[modelName] = deferredCategory->second;
    }
    return resolvedModel;
  }

  return nullptr;
}


DataModelRegistry::RegisteredModelCreatorsMap const &
DataModelRegistry::
registeredModelCreators() const
{
  return _registeredItemCreators;
}


DataModelRegistry::RegisteredModelsCategoryMap const &
DataModelRegistry::
registeredModelsCategoryAssociation() const
{
  return _registeredModelsCategory;
}


DataModelRegistry::CategoriesSet const &
DataModelRegistry::
categories() const
{
  return _categories;
}


TypeConverter
DataModelRegistry::
getTypeConverter(NodeDataType const & d1,
                 NodeDataType const & d2) const
{
  TypeConverterId converterId = std::make_pair(d1, d2);

  auto it = _registeredTypeConverters.find(converterId);

  if (it != _registeredTypeConverters.end())
  {
    return it->second;
  }

  return TypeConverter{};
}
