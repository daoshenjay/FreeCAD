#include <Python.h>
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>

#include <CXX/Extensions.hxx>
#include <CXX/Objects.hxx>

#include <App/Application.h>
#include <App/Document.h>

#include "DWGImporter.h"
#include "DWGReader.h"
#include "DWGText.h"
#include "DWGDimension.h"
#include "DWGLineGroup.h"
#include "DWGLineType.h"
#include "DWGShapeViewer.h"
#include "DWGSurfaceMesh.h"
#include "MultiInstanceTest.h"
#include "PropertyShapeBinary.h"

// ============================ 导出功能 BEGIN ============================
// DWG 导出：FreeCAD 对象 -> DWG（LibreDWG 写接口，不经 DXF）。
// 说明见 App/DWGCompose.h（对象 -> Entity）与 App/DWGWriter.h（Entity -> .dwg）。
#include "DWGCompose.h"
#include "DWGWriter.h"

#include <App/DocumentObjectPy.h>
// ============================ 导出功能 END ==============================


namespace SWDWG
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("SWDWG")
    {
        add_varargs_method("readDWG", &Module::readDWG,
                           "readDWG(filename) -> dict\n"
                           "\n"
                           "Read a DWG file using LibreDWG and return a nested\n"
                           "dictionary describing the drawing contents (layers,\n"
                           "styles, dimension styles, linetypes, blocks and\n"
                           "geometric entities).");
        add_varargs_method("isLibredwgAvailable", &Module::isLibredwgAvailable,
                           "isLibredwgAvailable() -> bool\n"
                           "\n"
                           "Returns True when the LibreDWG library was linked in.");
        // ======================== 导出功能 BEGIN ========================
        // DWG 导出（AutoCAD 2018 / AC1032）。objects 为空表示导出当前文档
        // 全部对象；返回 {"entities","layers","skipped"} 统计。
        add_varargs_method("writeDWG", &Module::writeDWG,
                           "writeDWG(filename, objects=[]) -> dict\n"
                           "\n"
                           "Write FreeCAD objects into a DWG file (AutoCAD 2018)\n"
                           "through LibreDWG's writer API.  No DXF file is used.\n"
                           "Returns a dict with the entity / layer / skipped\n"
                           "counts.");
        // ======================== 导出功能 END ==========================
        add_varargs_method("open", &Module::open,
                           "open(filename) -> None\n"
                           "\n"
                           "Open (import) a DWG file into a new document.\n"
                           "Implemented natively in C++ (LibreDWG + OCC), no\n"
                           "Python geometry wrapping involved.");
        add_varargs_method("insert", &Module::insert,
                           "insert(filename, docname) -> None\n"
                           "\n"
                           "Insert a DWG file into an existing FreeCAD document.\n"
                           "Implemented natively in C++ (LibreDWG + OCC), no\n"
                           "Python geometry wrapping involved.");
        add_varargs_method("viewShx",
                           &Module::viewShx,
                           "viewShx(shxfile [, docname]) -> None\n"
                           "\n"
                           "Draw every shape of a compiled AutoCAD shape file\n"
                           "(.shx - font such as txt.shx or symbol library such\n"
                           "as ltypeshp.shx) on a grid in a FreeCAD document,\n"
                           "one Part::Feature per glyph.  Uses the same parser\n"
                           "as the L2 complex-linetype restore.");

        initialize("This module is the SWDWG module.");  // register with Python
    }

    virtual ~Module()
    {}

private:
    Py::Object readDWG(const Py::Tuple& args)
    {
        char* filename = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "s", &filename)) {
            throw Py::Exception();
        }

        DWGReader reader;
        std::string error;
        if (!reader.read(filename, error)) {
            throw Py::RuntimeError(error);
        }

        return Py::Object(reader.result());
    }

    Py::Object isLibredwgAvailable(const Py::Tuple& args)
    {
        return Py::True();
    }

    // ============================ 导出功能 BEGIN ============================
    //! writeDWG(filename, objects = []) -> dict
    //!
    //! 导出功能：把 objects（DocumentObject 列表；空 = 当前文档全部对象）写成
    //! AutoCAD 2018 DWG。纯 C++ 管线：DWGCompose（对象 -> Entity）+
    //! DWGWriter（Entity -> dwg_add_* -> 文件）。
    Py::Object writeDWG(const Py::Tuple& args)
    {
        char* filename = nullptr;
        PyObject* pyObjects = Py_None;
        if (!PyArg_ParseTuple(args.ptr(), "s|O:writeDWG", &filename, &pyObjects)) {
            throw Py::Exception();
        }
        if (!filename || !*filename) {
            throw Py::ValueError("writeDWG: filename is empty");
        }

        std::vector<App::DocumentObject*> objects;
        App::Document* doc = nullptr;
        if (pyObjects && pyObjects != Py_None) {
            Py::Sequence sequence(pyObjects);
            for (const auto& item : sequence) {
                if (PyObject_TypeCheck(item.ptr(), &App::DocumentObjectPy::Type)) {
                    auto* obj = static_cast<App::DocumentObjectPy*>(item.ptr())
                                    ->getDocumentObjectPtr();
                    if (obj) {
                        objects.push_back(obj);
                        if (!doc) {
                            doc = obj->getDocument();
                        }
                    }
                }
            }
        }
        if (!doc) {
            doc = App::GetApplication().getActiveDocument();
        }
        if (!doc) {
            throw Py::RuntimeError("writeDWG: no document to export");
        }

        std::string error;
        std::vector<SWDWG::Entity> entities;
        std::vector<SWDWG::DWGOutLayer> layers;
        std::vector<SWDWG::DWGOutLinetype> linetypes;
        const int skippedObjects = SWDWG::DWGCompose::compose(
            doc, objects, entities, layers, linetypes, error);
        if (!error.empty()) {
            throw Py::RuntimeError(error);
        }

        SWDWG::DWGWriteStats stats;
        if (!SWDWG::DWGWriter::writeDWG(filename, entities, layers, linetypes,
                                        error, stats)) {
            throw Py::RuntimeError(error);
        }

        Py::Dict result;
        result.setItem("entities", Py::Long(stats.entities));
        result.setItem("layers", Py::Long(stats.layers));
        result.setItem("skipped", Py::Long(stats.skipped + skippedObjects));
        return result;
    }
    // ============================ 导出功能 END ==============================

    Py::Object viewShx(const Py::Tuple& args)
    {
        char* filename = nullptr;
        char* docname = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "s|s:viewShx", &filename,
                              &docname)) {
            throw Py::Exception();
        }

        App::Document* doc = nullptr;
        if (docname && *docname) {
            doc = App::GetApplication().getDocument(docname);
        }
        if (!doc) {
            doc = App::GetApplication().getActiveDocument();
        }
        if (!doc) {
            doc = App::GetApplication().newDocument(
                "SHX");
        }
        if (!doc) {
            throw Py::RuntimeError("SWDWG: no document to draw into");
        }

        std::string error;
        if (!viewShxFont(doc, filename, error)) {
            throw Py::RuntimeError(error);
        }
        return Py::None();
    }

    Py::Object open(const Py::Tuple& args)
    {
        char* filename = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "s:open", &filename)) {
            throw Py::Exception();
        }

        // Name the document after the DWG file, so File > Open on a .dwg
        // produces a document - and a default .fcstd save name - matching the
        // drawing's own name instead of a generic "DWG".  newDocument()
        // unique-ifies the name, so importing the same file twice keeps both
        // documents distinct.  (SWDWG_Import in Gui/Command.cpp applies the
        // same rule for the workbench toolbar command.)
        const std::string base = Base::FileInfo(filename).fileNamePure();
        App::Document* doc = App::GetApplication().newDocument(
            base.empty() ? "DWG" : base.c_str());
        if (!doc) {
            throw Py::RuntimeError("SWDWG: could not create a new document");
        }

        std::string error;
        int created = 0;
        int skipped = 0;
        if (!DWGImporter::importDWG(doc, filename, error, created, skipped)) {
            throw Py::RuntimeError(error);
        }
        return Py::None();
    }

    Py::Object insert(const Py::Tuple& args)
    {
        char* filename = nullptr;
        char* docname = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "s|s:insert", &filename, &docname)) {
            throw Py::Exception();
        }

        App::Document* doc = nullptr;
        if (docname && *docname) {
            doc = App::GetApplication().getDocument(docname);
        }
        if (!doc) {
            doc = App::GetApplication().getActiveDocument();
        }
        // ===================== 导入修复 BEGIN =====================
        // 文件 > 导入（module_io 的 "insert"）在没有任何文档时也会走到这里。
        // FreeCAD 的约定是"导入器自己创建文档"（见 Gui::Application::importFrom
        // 里 "The import function then may create a new document" 的注释），
        // Draft 的 importDWG.insert 也是这么做的。这里按 DWG 文件名建一个，
        // 命名规则与 open() 保持一致；已有文档时行为完全不变。
        if (!doc) {
            const std::string base = Base::FileInfo(filename).fileNamePure();
            doc = App::GetApplication().newDocument(
                base.empty() ? "DWG" : base.c_str());
            if (doc) {
                SWDWG_LOG("SWDWG: insert without a document -> created '%s'\n",
                          doc->getName());
            }
        }
        // ===================== 导入修复 END =======================
        if (!doc) {
            throw Py::RuntimeError(
                "SWDWG: no active document to insert the drawing into");
        }

        std::string error;
        int created = 0;
        int skipped = 0;
        if (!DWGImporter::importDWG(doc, filename, error, created, skipped)) {
            throw Py::RuntimeError(error);
        }
        return Py::None();
    }
};

PyObject* initModule()
{
    // Register the custom document object types in the type system so that
    // the C++ template addObject<SWDWG::DWGText>() / addObject<SWDWG::DWGDimension>()
    // works.  (The Python name registration is not needed: the importer creates
    // the objects directly from C++.)
    SWDWG::PropertyShapeBinary::init();
    SWDWG::DWGText::init();
    SWDWG::DWGDimension::init();
    SWDWG::DWGLineGroup::init();
    SWDWG::DWGLineType::init();
    SWDWG::DWGSurfaceMesh::init();
    SWDWG::MultiInstanceTest::init();
    return Base::Interpreter().addModule(new Module);
}


}  // namespace SWDWG


/* Python entry */
PyMOD_INIT_FUNC(SWDWG)
{
    // ADD YOUR CODE HERE
    //
    //
    PyObject* mod = SWDWG::initModule();
    SWDWG_LOG("Loading SWDWG module... done\n");
    PyMOD_Return(mod);
}
