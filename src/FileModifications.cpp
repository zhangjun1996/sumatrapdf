/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include "ScopedWin.h"

#include "FileUtil.h"
#include "SquareTreeParser.h"

#include "BaseEngine.h"
#include "FileModifications.h"
#include "Version.h"

/*
The following format (SumatraPDF Modifications eXtensible) is used for
storing file modifications for file formats which don't allow to save
such modifications portably within the file structure (i.e. currently
any format but PDF). The format uses SquareTree syntax (using its INI
serialization for better interoperability):

[@meta]
version = 2.3
filesize = 98765
timestamp = 2013-03-09T12:34:56Z

[highlight]
page = 1
rect = 10 10 100 100
color = #ff0000
opacity = 0.8

[annotType]
page = no
rect = x y w h
color = #rrggbb
opacity = 1

...

[@update]
version = 2.3
filesize = 98765
timestamp = 2013-03-10T05:43:21Z

...

(Currently, the only supported modifications are adding annotations.)
*/

#define SMX_FILE_EXT L".smx"
#define SMX_CURR_VERSION CURR_VERSION_STRA

static Vec<PageAnnotation>* ParseFileModifications(const char* data) {
    if (!data)
        return nullptr;

    SquareTree sqt(data);
    if (!sqt.root || sqt.root->data.size() == 0)
        return nullptr;
    SquareTreeNode::DataItem& item = sqt.root->data.at(0);
    if (!item.isChild || !str::EqI(item.key, "@meta"))
        return nullptr;
    if (!item.value.child->GetValue("version")) {
        // don't check the version value - rather extend the format
        // in a way to ensure backwards compatibility
        return nullptr;
    }

    Vec<PageAnnotation>* list = new Vec<PageAnnotation>();
    for (SquareTreeNode::DataItem& i : sqt.root->data) {
        PageAnnotType type =
            str::EqI(i.key, "highlight")
                ? PageAnnotType::Highlight
                : str::EqI(i.key, "underline")
                      ? PageAnnotType::Underline
                      : str::EqI(i.key, "strikeout")
                            ? PageAnnotType::StrikeOut
                            : str::EqI(i.key, "squiggly") ? PageAnnotType::Squiggly : PageAnnotType::None;
        CrashIf(!i.isChild);
        if (PageAnnotType::None == type || !i.isChild)
            continue;

        int pageNo;
        geomutil::RectT<float> rect;
        PageAnnotation::Color color;
        float opacity;
        int r, g, b;

        SquareTreeNode* node = i.value.child;
        const char* value = node->GetValue("page");
        if (!value || !str::Parse(value, "%d%$", &pageNo))
            continue;
        value = node->GetValue("rect");
        if (!value || !str::Parse(value, "%f %f %f %f%$", &rect.x, &rect.y, &rect.dx, &rect.dy))
            continue;
        value = node->GetValue("color");
        if (!value || !str::Parse(value, "#%2x%2x%2x%$", &r, &g, &b))
            continue;
        value = node->GetValue("opacity");
        if (!value || !str::Parse(value, "%f%$", &opacity))
            opacity = 1.0f;
        color = PageAnnotation::Color((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)(255 * opacity));
        list->Append(PageAnnotation(type, pageNo, rect.Convert<double>(), color));
    }

    return list;
}

Vec<PageAnnotation>* LoadFileModifications(const WCHAR* filePath) {
    AutoFreeW modificationsPath(str::Join(filePath, SMX_FILE_EXT));
    OwnedData data(file::ReadFile(modificationsPath));
    return ParseFileModifications(data.data);
}

bool SaveFileModifictions(const WCHAR* filePath, Vec<PageAnnotation>* list) {
    if (!list) {
        return false;
    }

    AutoFreeW modificationsPath(str::Join(filePath, SMX_FILE_EXT));
    str::Str<char> data;
    size_t offset = 0;

    OwnedData prevData(file::ReadFile(modificationsPath));
    Vec<PageAnnotation>* prevList = ParseFileModifications(prevData.data);
    bool isUpdate = prevList != nullptr;
    if (isUpdate) {
        // in the case of an update, append changed annotations to the existing ones
        // (don't rewrite the existing ones in case they're by a newer version which
        // added annotation types and properties this version doesn't know anything about)
        for (; offset < prevList->size() && prevList->at(offset) == list->at(offset); offset++) {
            // do nothing
        }
        CrashIfDebugOnly(offset != prevList->size());
        data.AppendAndFree(prevData.StealData());
        delete prevList;
    } else {
        data.AppendFmt("# SumatraPDF: modifications to \"%S\"\r\n", path::GetBaseName(filePath));
    }
    data.Append("\r\n");

    if (list->size() == offset) {
        return true; // nothing (new) to save
    }

    data.AppendFmt("[@%s]\r\n", isUpdate ? "update" : "meta");
    data.AppendFmt("version = %s\r\n", SMX_CURR_VERSION);
    int64_t size = file::GetSize(filePath);
    if (0 <= size && size <= UINT_MAX) {
        data.AppendFmt("filesize = %u\r\n", (UINT)size);
    }
    SYSTEMTIME time;
    GetSystemTime(&time);
    data.AppendFmt("timestamp = %04d-%02d-%02dT%02d:%02d:%02dZ\r\n", time.wYear, time.wMonth, time.wDay, time.wHour,
                   time.wMinute, time.wSecond);
    data.Append("\r\n");

    for (size_t i = offset; i < list->size(); i++) {
        PageAnnotation& annot = list->at(i);
        switch (annot.type) {
            case PageAnnotType::Highlight:
                data.Append("[highlight]\r\n");
                break;
            case PageAnnotType::Underline:
                data.Append("[underline]\r\n");
                break;
            case PageAnnotType::StrikeOut:
                data.Append("[strikeout]\r\n");
                break;
            case PageAnnotType::Squiggly:
                data.Append("[squiggly]\r\n");
                break;
            default:
                continue;
        }
        data.AppendFmt("page = %d\r\n", annot.pageNo);
        data.AppendFmt("rect = %g %g %g %g\r\n", annot.rect.x, annot.rect.y, annot.rect.dx, annot.rect.dy);
        data.AppendFmt("color = #%02x%02x%02x\r\n", annot.color.r, annot.color.g, annot.color.b);
        data.AppendFmt("opacity = %g\r\n", annot.color.a / 255.f);
        data.Append("\r\n");
    }
    data.RemoveAt(data.size() - 2, 2);

    return file::WriteFile(modificationsPath, data.LendData(), data.size());
}

bool IsModificationsFile(const WCHAR* filePath) {
    if (!str::EndsWithI(filePath, SMX_FILE_EXT)) {
        return false;
    }
    AutoFreeW origPath(str::DupN(filePath, str::Len(filePath) - str::Len(SMX_FILE_EXT)));
    return file::Exists(origPath);
}
