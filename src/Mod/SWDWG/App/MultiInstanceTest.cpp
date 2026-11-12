//! Demo document object for SoMultipleInstance instancing - implementation.

#include "PreCompiled.h"

#include "MultiInstanceTest.h"

using namespace SWDWG;

PROPERTY_SOURCE(SWDWG::MultiInstanceTest, App::DocumentObject)

MultiInstanceTest::MultiInstanceTest()
{
    ADD_PROPERTY_TYPE(Rows,
                      (8),
                      "Instances",
                      App::Prop_None,
                      "Number of rows in the instance grid");
    ADD_PROPERTY_TYPE(Cols,
                      (8),
                      "Instances",
                      App::Prop_None,
                      "Number of columns in the instance grid");
    ADD_PROPERTY_TYPE(Spacing,
                      (10.0f),
                      "Instances",
                      App::Prop_None,
                      "Distance between adjacent instances (world units)");
    ADD_PROPERTY_TYPE(Template,
                      (0),
                      "Instances",
                      App::Prop_None,
                      "Template shape: 0 = diamond, 1 = plus, 2 = octagon disc");
}

MultiInstanceTest::~MultiInstanceTest() = default;

void MultiInstanceTest::onChanged(const App::Property* prop)
{
    App::DocumentObject::onChanged(prop);
}
