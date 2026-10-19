#include "mso/ui/about_dialog.hpp"

#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "mso/generated/dependencies.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("About Music Sample Organizer"));
    resize(640, 560);
    auto* text = new QTextBrowser(this);
    text->setOpenExternalLinks(false);  // offline application: links are shown, not followed
    text->setHtml(html());
    text->setAccessibleName(tr("About and third-party notices"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(text, 1);
    layout->addWidget(buttons);
}

QString AboutDialog::html() {
    QString body = tr("<h3>Music Sample Organizer %1</h3>"
                      "<p>An offline sample library browser for music producers.</p>"
                      "<p>Licensed under the GNU General Public License, version 3 (%2). "
                      "This program comes with ABSOLUTELY NO WARRANTY.</p>")
                       .arg(DisplayFormat::from_utf8(generated::kProjectVersion),
                            DisplayFormat::from_utf8(generated::kProjectLicense));
    body += tr("<h4>Third-party components</h4>");
    body += QStringLiteral("<table cellspacing=\"4\">");
    for (const generated::DependencyNotice& dependency : generated::kDependencies) {
        body += tr("<tr><td><b>%1</b> %2</td><td>%3</td><td>%4</td></tr>")
                    .arg(DisplayFormat::from_utf8(dependency.name).toHtmlEscaped(),
                         DisplayFormat::from_utf8(dependency.version).toHtmlEscaped(),
                         DisplayFormat::from_utf8(dependency.license).toHtmlEscaped(),
                         DisplayFormat::from_utf8(dependency.purpose).toHtmlEscaped());
    }
    body += QStringLiteral("</table>");
    body += tr("<h4>License notices</h4>");
    for (const generated::DependencyNotice& dependency : generated::kDependencies) {
        if (dependency.license_text.empty()) continue;
        body += tr("<p><b>%1</b> (%2)</p><pre>%3</pre>")
                    .arg(DisplayFormat::from_utf8(dependency.name).toHtmlEscaped(),
                         DisplayFormat::from_utf8(dependency.url).toHtmlEscaped(),
                         DisplayFormat::from_utf8(dependency.license_text).toHtmlEscaped());
    }
    return body;
}

}  // namespace mso::ui
