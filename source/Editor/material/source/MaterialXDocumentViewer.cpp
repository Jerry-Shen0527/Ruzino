#include "MCore/MaterialXDocumentViewer.hpp"

#include <MaterialXFormat/XmlIo.h>

#include <sstream>

RUZINO_NAMESPACE_OPEN_SCOPE

MaterialXDocumentViewer::MaterialXDocumentViewer(
    MaterialXNodeTree* node_tree,
    const std::string& title)
    : TextEditorWidget(title),
      node_tree_(node_tree)
{
    // Set to XML language and read-only mode
    SetLanguage(Language::XML);
    SetReadOnly(true);
    SetFontSize(14.0f);

    // Initial document refresh
    RefreshDocument();
}

void MaterialXDocumentViewer::RefreshDocument()
{
    if (!node_tree_) {
        UpdateText("<!-- No MaterialX node tree available -->");
        return;
    }

    try {
        // Get the MaterialX document from the node tree
        auto doc = node_tree_->_graphDoc;

        if (!doc) {
            UpdateText("<!-- No MaterialX document available -->");
            return;
        }

        // Convert document to XML string
        mx::XmlWriteOptions writeOptions;
        writeOptions.writeXIncludeEnable = false;
        writeOptions.elementPredicate = node_tree_->getElementPredicate();

        std::stringstream ss;
        mx::writeToXmlStream(doc, ss, &writeOptions);

        // Update the text editor with the XML content
        UpdateText(ss.str());
    }
    catch (std::exception& e) {
        std::string error_msg = "<!-- Error generating MaterialX document:\n";
        error_msg += e.what();
        error_msg += "\n-->";
        UpdateText(error_msg);
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
