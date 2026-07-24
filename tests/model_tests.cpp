#include "launchpad_model.h"
#include "launchpad_layout.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path test_store_path(std::string_view suffix) {
    return std::filesystem::temp_directory_path() /
        ("windows-launchpad-model-" + std::string(suffix) + ".store");
}

bool write_test_store(
    const std::filesystem::path& path,
    std::string_view contents) {
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    stream << contents;
    return static_cast<bool>(stream);
}

} // namespace

int main() {
    using namespace launchpad;

    expect(contains_insensitive(L"Visual Studio", L"studio"),
           "ASCII search is case-insensitive");
    expect(contains_insensitive(L"Калькулятор", L"КАЛЬК"),
           "Cyrillic search is case-insensitive");
    expect(!contains_insensitive(L"Проводник", L"paint"),
           "unrelated query is rejected");
    expect(contains_insensitive(L"Anything", L""),
           "empty query matches");

    const int exact_firefox =
        search_match_score(L"Firefox", L"firefox");
    const int prefix_firefox =
        search_match_score(L"Firefox Developer Edition", L"firefox");
    const int substring_firefox =
        search_match_score(L"Mozilla Firefox", L"firefox");
    expect(exact_firefox > prefix_firefox &&
               prefix_firefox > substring_firefox,
           "exact search ranks above prefix and substring matches");
    expect(search_match_score(
               L"Visual Studio Code",
               L"vsc") != kNoSearchMatch,
           "word-initial acronym matches an application name");
    expect(search_match_score(
               L"Firefox",
               L"firfox") != kNoSearchMatch,
           "ordered fuzzy search tolerates a missing character");
    expect(search_match_score(
               L"Firefox",
               L"fierfox") != kNoSearchMatch,
           "bigram search tolerates an adjacent transposition");
    expect(search_match_score(
               L"Visual Studio Code",
               L"studio visual") != kNoSearchMatch,
           "multi-token search does not require query token order");
    expect(search_match_score(
               L"Visual Studio Code",
               L"vs") != kNoSearchMatch,
           "short word-initial acronyms remain useful");
    expect(search_match_score(
               L"Visual Studio Code",
               L"zx") == kNoSearchMatch,
           "unrelated two-character queries do not use noisy fuzzy matching");
    expect(search_match_score(
               L"Калькулятор",
               L"КАЛЬК") != kNoSearchMatch,
           "fuzzy scoring keeps Cyrillic case-insensitive substring search");
    expect(search_match_score(
               L"Проводник",
               L"paint") == kNoSearchMatch,
           "fuzzy scoring rejects unrelated Unicode names");
    const int deterministic_score = search_match_score(
        L"Microsoft Visual Studio Code",
        L"vsc");
    expect(
        deterministic_score ==
            search_match_score(
                L"Microsoft Visual Studio Code",
                L"vsc"),
        "search scoring is deterministic");
    expect(normalize_search_text(L"  Visual-Studio__Code  ") ==
               L"visual studio code",
           "search normalization folds case and separators");

    expect(is_supported_app_extension(L".LNK"),
           "LNK extension is accepted case-insensitively");
    expect(is_supported_app_extension(L".appref-ms"),
           "ClickOnce shortcuts are accepted");
    expect(!is_supported_app_extension(L".txt"),
           "documents are not treated as applications");

    expect(page_count(0) == 1, "empty state still has one page");
    expect(page_count(35) == 1, "one full grid is one page");
    expect(page_count(36) == 2, "overflow creates a second page");
    expect(page_count(70) == 2, "two full grids are two pages");
    expect(!should_show_page_indicator(0),
           "empty state has no decorative page dot");
    expect(!should_show_page_indicator(35),
           "a single page has no page indicator");
    expect(should_show_page_indicator(36),
           "page indicator appears for multiple pages");

    LayoutDocument layout;
    const std::vector<std::pair<std::wstring, std::wstring>> apps{
        {L"C:\\Apps\\Alpha.lnk", L"Alpha"},
        {L"C:\\Apps\\Бета.lnk", L"Бета"},
        {L"C:\\Apps\\Gamma.lnk", L"Gamma"},
    };
    expect(layout.reconcile(apps),
           "new applications populate an empty layout");
    expect(layout.items().size() == 3,
           "all applications are represented once");
    expect(layout.create_folder(0, 1, L"Папка"),
           "dropping one app on another creates a folder");
    expect(layout.items().size() == 2 &&
               layout.items()[0].kind == LayoutItemKind::folder &&
               layout.items()[0].children.size() == 2,
           "folder replaces its two source applications");
    expect(layout.add_app_to_folder(1, 0),
           "an app can be dropped into an existing folder");
    expect(layout.items().size() == 1 &&
               layout.items()[0].children.size() == 3,
           "folder owns all dropped applications");
    expect(layout.move_folder_app(0, 2, 0),
           "folder applications can be reordered");
    expect(layout.items()[0].children.front() ==
               L"C:\\Apps\\Gamma.lnk",
           "folder reorder keeps stable application paths");
    expect(layout.move_folder_app(0, 0, 2),
           "folder applications can be reordered in reverse");
    expect(layout.items()[0].children ==
               std::vector<std::wstring>{
                   L"C:\\Apps\\Бета.lnk",
                   L"C:\\Apps\\Alpha.lnk",
                   L"C:\\Apps\\Gamma.lnk",
               },
           "reverse folder reorder uses the final destination slot");
    expect(!layout.move_folder_app(0, 1, 1),
           "moving a folder application onto itself is a no-op");
    expect(!layout.move_folder_app(0, 3, 0),
           "an invalid folder source is rejected");
    expect(!layout.move_folder_app(0, 0, 3),
           "an invalid folder destination is rejected");

    const std::filesystem::path folder_roundtrip_path =
        test_store_path("folder-roundtrip");
    std::filesystem::remove(folder_roundtrip_path);
    expect(save_layout(folder_roundtrip_path, layout),
           "folder application order can be saved");
    LayoutDocument folder_roundtrip_layout;
    expect(load_layout(
               folder_roundtrip_path,
               folder_roundtrip_layout),
           "folder application order can be loaded");
    expect(folder_roundtrip_layout.items() == layout.items(),
           "folder application order survives a roundtrip");
    std::filesystem::remove(folder_roundtrip_path);

    LayoutDocument dissolving_folder;
    dissolving_folder.reconcile({
        {L"C:\\Apps\\One.lnk", L"One"},
        {L"C:\\Apps\\Two.lnk", L"Two"},
    });
    expect(dissolving_folder.create_folder(0, 1, L"Pair"),
           "two applications can form a dissolving folder fixture");
    expect(dissolving_folder.reconcile({
               {L"C:\\Apps\\Two.lnk", L"Two renamed"},
           }),
           "removing one of two folder children changes the layout");
    expect(dissolving_folder.items().size() == 1 &&
               dissolving_folder.items()[0].kind ==
                   LayoutItemKind::app &&
               dissolving_folder.items()[0].app_path ==
                   L"C:\\Apps\\Two.lnk" &&
               dissolving_folder.items()[0].name ==
                   L"Two renamed",
           "a one-child folder dissolves into its remaining application");

    const std::vector<std::pair<std::wstring, std::wstring>>
        extraction_apps{
            {L"C:\\Apps\\One.lnk", L"One"},
            {L"C:\\Apps\\Two.lnk", L"Two"},
            {L"C:\\Apps\\Three.lnk", L"Three"},
            {L"C:\\Apps\\Four.lnk", L"Four"},
        };
    LayoutDocument extraction_layout;
    extraction_layout.reconcile(extraction_apps);
    expect(extraction_layout.create_folder(0, 1, L"Group"),
           "folder extraction fixture can create a folder");
    expect(extraction_layout.add_app_to_folder(1, 0),
           "folder extraction fixture can add a third child");
    expect(extraction_layout.extract_folder_app(
               0,
               1,
               extraction_apps),
           "a child can be extracted from a three-application folder");
    expect(extraction_layout.items().size() == 3 &&
               extraction_layout.items()[0].kind ==
                   LayoutItemKind::folder &&
               extraction_layout.items()[0].children ==
                   std::vector<std::wstring>{
                       L"C:\\Apps\\Two.lnk",
                       L"C:\\Apps\\Three.lnk",
                   } &&
               extraction_layout.items()[1].kind ==
                   LayoutItemKind::app &&
               extraction_layout.items()[1].app_path ==
                   L"C:\\Apps\\One.lnk" &&
               extraction_layout.items()[2].app_path ==
                   L"C:\\Apps\\Four.lnk",
           "extraction preserves remaining child order and inserts "
           "the app beside its folder");
    const LayoutDocument extraction_before_invalid =
        extraction_layout;
    expect(!extraction_layout.extract_folder_app(
               0,
               9,
               extraction_apps) &&
               extraction_layout.items() ==
                   extraction_before_invalid.items(),
           "an invalid folder child extraction is a no-op");
    expect(!extraction_layout.extract_folder_app(
               9,
               0,
               extraction_apps) &&
               extraction_layout.items() ==
                   extraction_before_invalid.items(),
           "an invalid folder extraction target is a no-op");
    const LayoutDocument extraction_before_missing_metadata =
        extraction_layout;
    expect(!extraction_layout.extract_folder_app(
               0,
               0,
               {
                   {L"C:\\Apps\\One.lnk", L"One"},
                   {L"C:\\Apps\\Three.lnk", L"Three"},
                   {L"C:\\Apps\\Four.lnk", L"Four"},
               }) &&
               extraction_layout.items() ==
                   extraction_before_missing_metadata.items(),
           "missing selected application metadata leaves extraction "
           "exactly unchanged");
    expect(!extraction_layout.extract_folder_app(
               0,
               0,
               {
                   {L"C:\\Apps\\One.lnk", L"One"},
                   {L"C:\\Apps\\Two.lnk", L"Two"},
                   {L"C:\\Apps\\Four.lnk", L"Four"},
               }) &&
               extraction_layout.items() ==
                   extraction_before_missing_metadata.items(),
           "missing remaining application metadata leaves a pair "
           "extraction exactly unchanged");

    LayoutDocument pair_extraction_layout;
    pair_extraction_layout.reconcile({
        {L"C:\\Apps\\One.lnk", L"One"},
        {L"C:\\Apps\\Two.lnk", L"Two"},
    });
    expect(pair_extraction_layout.create_folder(0, 1, L"Pair"),
           "pair extraction fixture can create a folder");
    expect(pair_extraction_layout.extract_folder_app(
               0,
               0,
               {
                   {L"C:\\Apps\\One.lnk", L"One"},
                   {L"C:\\Apps\\Two.lnk", L"Two"},
               }),
           "a child can be extracted from a two-application folder");
    expect(pair_extraction_layout.items() ==
               std::vector<LayoutItem>{
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"One",
                       .app_path = L"C:\\Apps\\One.lnk",
                   },
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"Two",
                       .app_path = L"C:\\Apps\\Two.lnk",
                   },
               },
           "extracting from a pair dissolves the folder into two "
           "top-level applications");

    LayoutDocument second_pair_extraction_layout;
    second_pair_extraction_layout.reconcile({
        {L"C:\\Apps\\One.lnk", L"One"},
        {L"C:\\Apps\\Two.lnk", L"Two"},
    });
    expect(second_pair_extraction_layout.create_folder(
               0,
               1,
               L"Pair"),
           "second-child pair extraction fixture can create a folder");
    expect(second_pair_extraction_layout.extract_folder_app(
               0,
               1,
               {
                   {L"C:\\Apps\\One.lnk", L"One"},
                   {L"C:\\Apps\\Two.lnk", L"Two"},
               }),
           "the second child can be extracted from a two-application "
           "folder");
    expect(second_pair_extraction_layout.items() ==
               std::vector<LayoutItem>{
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"Two",
                       .app_path = L"C:\\Apps\\Two.lnk",
                   },
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"One",
                       .app_path = L"C:\\Apps\\One.lnk",
                   },
               },
           "extracting the second pair child keeps the remaining app "
           "first and the extracted app second");

    const std::vector<std::pair<std::wstring, std::wstring>>
        canonical_case_apps{
            {L"C:\\Apps\\CaseOne.LNK", L"Canonical One"},
            {L"C:\\Apps\\CaseTwo.LNK", L"Canonical Two"},
        };
    LayoutDocument canonical_case_extraction_layout;
    canonical_case_extraction_layout.reconcile(canonical_case_apps);
    expect(canonical_case_extraction_layout.create_folder(
               0,
               1,
               L"Case pair"),
           "case-insensitive extraction fixture can create a folder");
    canonical_case_extraction_layout.items()[0].children = {
        L"c:\\apps\\casetwo.lnk",
        L"c:\\apps\\caseone.lnk",
    };
    expect(canonical_case_extraction_layout.extract_folder_app(
               0,
               1,
               canonical_case_apps),
           "folder extraction resolves application metadata "
           "case-insensitively");
    expect(canonical_case_extraction_layout.items() ==
               std::vector<LayoutItem>{
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"Canonical Two",
                       .app_path = L"C:\\Apps\\CaseTwo.LNK",
                   },
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"Canonical One",
                       .app_path = L"C:\\Apps\\CaseOne.LNK",
                   },
               },
           "case-insensitive extraction emits canonical paths and "
           "names from application metadata");

    LayoutDocument break_extraction_layout;
    break_extraction_layout.reconcile({
        {L"C:\\Apps\\One.lnk", L"One"},
        {L"C:\\Apps\\Two.lnk", L"Two"},
        {L"C:\\Apps\\Three.lnk", L"Three"},
    });
    expect(break_extraction_layout.create_folder(0, 1, L"Pair") &&
               break_extraction_layout.move_item_to_new_page(1),
           "page-break extraction fixture can be arranged");
    expect(break_extraction_layout.extract_folder_app(
               0,
               1,
               {
                   {L"C:\\Apps\\One.lnk", L"One"},
                   {L"C:\\Apps\\Two.lnk", L"Two"},
                   {L"C:\\Apps\\Three.lnk", L"Three"},
               }),
           "an app can be extracted beside a page break");
    expect(break_extraction_layout.items() ==
               std::vector<LayoutItem>{
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"Two",
                       .app_path = L"C:\\Apps\\Two.lnk",
                   },
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"One",
                       .app_path = L"C:\\Apps\\One.lnk",
                   },
                   LayoutItem{
                       .kind = LayoutItemKind::page_break,
                   },
                   LayoutItem{
                       .kind = LayoutItemKind::app,
                       .name = L"Three",
                       .app_path = L"C:\\Apps\\Three.lnk",
                   },
               },
           "folder extraction keeps exact app order and canonical "
           "paths before the explicit page break");

    LayoutDocument reorder_layout;
    reorder_layout.reconcile(apps);
    expect(reorder_layout.move_item(0, 1),
           "top-level applications can move to the right");
    expect(reorder_layout.items()[0].name == L"Бета" &&
               reorder_layout.items()[1].name == L"Alpha",
           "top-level reorder uses the final destination slot");
    const std::vector<LayoutItem> reordered_apps =
        reorder_layout.items();
    expect(!reorder_layout.move_item(1, 1) &&
               reorder_layout.items() == reordered_apps,
           "dropping a top-level application on its current slot "
           "does not mutate the layout");
    expect(!reorder_layout.move_item(99, 0) &&
               !reorder_layout.move_item(0, 99) &&
               reorder_layout.items() == reordered_apps,
           "invalid top-level reorder indices leave the layout "
           "exactly unchanged");

    LayoutDocument folder_item_reorder_layout;
    folder_item_reorder_layout.reconcile({
        {L"C:\\Apps\\One.lnk", L"One"},
        {L"C:\\Apps\\Two.lnk", L"Two"},
        {L"C:\\Apps\\Three.lnk", L"Three"},
        {L"C:\\Apps\\Four.lnk", L"Four"},
    });
    expect(folder_item_reorder_layout.create_folder(
               0,
               1,
               L"Pair"),
           "top-level folder reorder fixture can create a folder");
    const LayoutItem pair_folder =
        folder_item_reorder_layout.items()[0];
    expect(folder_item_reorder_layout.move_item(0, 2),
           "a top-level folder can move after applications");
    expect(folder_item_reorder_layout.items()[0].name == L"Three" &&
               folder_item_reorder_layout.items()[1].name == L"Four" &&
               folder_item_reorder_layout.items()[2] == pair_folder,
           "moving a folder right shifts intervening applications "
           "left without changing folder contents");
    expect(folder_item_reorder_layout.move_item(2, 0),
           "a top-level folder can move back before applications");
    expect(folder_item_reorder_layout.items()[0] == pair_folder &&
               folder_item_reorder_layout.items()[1].name == L"Three" &&
               folder_item_reorder_layout.items()[2].name == L"Four",
           "moving a folder left shifts intervening applications "
           "right and restores the original order");

    LayoutDocument focused_folder_reorder_layout;
    focused_folder_reorder_layout.reconcile({
        {L"C:\\Apps\\One.lnk", L"One"},
        {L"C:\\Apps\\Two.lnk", L"Two"},
        {L"C:\\Apps\\Three.lnk", L"Three"},
        {L"C:\\Apps\\Four.lnk", L"Four"},
    });
    expect(focused_folder_reorder_layout.create_folder(
               0,
               1,
               L"Group") &&
               focused_folder_reorder_layout.add_app_to_folder(1, 0) &&
               focused_folder_reorder_layout.add_app_to_folder(1, 0),
           "folder child reorder fixture can collect four applications");
    const std::vector<std::wstring> original_child_order{
        L"C:\\Apps\\Two.lnk",
        L"C:\\Apps\\One.lnk",
        L"C:\\Apps\\Three.lnk",
        L"C:\\Apps\\Four.lnk",
    };
    expect(focused_folder_reorder_layout.items()[0].children ==
               original_child_order,
           "folder child reorder fixture starts in deterministic order");
    expect(focused_folder_reorder_layout.move_folder_app(0, 1, 3),
           "a folder child can move from the middle to the last slot");
    expect(focused_folder_reorder_layout.items()[0].children ==
               std::vector<std::wstring>{
                   L"C:\\Apps\\Two.lnk",
                   L"C:\\Apps\\Three.lnk",
                   L"C:\\Apps\\Four.lnk",
                   L"C:\\Apps\\One.lnk",
               },
           "moving a child right shifts every intervening child left");
    expect(focused_folder_reorder_layout.move_folder_app(0, 3, 1),
           "a folder child can move from the last slot back to the middle");
    expect(focused_folder_reorder_layout.items()[0].children ==
               original_child_order,
           "moving a child left shifts intervening children right "
           "and restores the original order");
    const std::vector<LayoutItem> focused_folder_before_invalid =
        focused_folder_reorder_layout.items();
    expect(!focused_folder_reorder_layout.move_folder_app(0, 2, 2) &&
               !focused_folder_reorder_layout.move_folder_app(0, 9, 0) &&
               !focused_folder_reorder_layout.move_folder_app(0, 0, 9) &&
               focused_folder_reorder_layout.items() ==
                   focused_folder_before_invalid,
           "self and invalid folder-child reorders leave the layout "
           "exactly unchanged");

    const std::vector<std::pair<std::wstring, std::wstring>>
        folder_target_apps{
            {L"C:\\Apps\\One.lnk", L"One"},
            {L"C:\\Apps\\Two.lnk", L"Two"},
            {L"C:\\Apps\\Three.lnk", L"Three"},
            {L"C:\\Apps\\Four.lnk", L"Four"},
        };
    LayoutDocument target_layout;
    target_layout.reconcile(folder_target_apps);
    expect(target_layout.move_item_to_new_page(2),
           "an item can move to a newly created last page");
    expect(target_layout.items().size() == 5 &&
               target_layout.items()[3].kind ==
                   LayoutItemKind::page_break &&
               target_layout.items()[4].name == L"Three",
           "a page break is stored before the moved item");
    expect(target_layout.create_folder(0, 4, L"Target folder"),
           "an app can create a folder around a target on another page");
    expect(target_layout.items().size() == 4 &&
               target_layout.items()[2].kind ==
                   LayoutItemKind::page_break &&
               target_layout.items()[3].kind ==
                   LayoutItemKind::folder &&
               target_layout.items()[3].children ==
                   std::vector<std::wstring>{
                       L"C:\\Apps\\Three.lnk",
                       L"C:\\Apps\\One.lnk",
                   },
           "folder creation keeps the target position and page");

    std::vector<std::pair<std::wstring, std::wstring>> page_apps;
    page_apps.reserve(35);
    for (std::size_t index = 0; index < 35; ++index) {
        const std::wstring number = std::to_wstring(index);
        page_apps.emplace_back(
            L"C:\\Apps\\Page" + number + L".lnk",
            L"Page " + number);
    }
    LayoutDocument paged_layout;
    expect(paged_layout.reconcile(page_apps),
           "page test applications populate the layout");
    expect(paged_layout.move_item_to_new_page(34),
           "the last item of a full grid can create page two");
    expect(paged_layout.items().size() == 36 &&
               paged_layout.items()[34].kind ==
                   LayoutItemKind::page_break &&
               paged_layout.items()[35].app_path ==
                   page_apps[34].first,
           "35 items persist as 34 items, a break, and one item");
    expect(!paged_layout.move_item_to_new_page(35),
           "moving the only last-page item to a new last page is a no-op");
    expect(!paged_layout.reconcile(page_apps),
           "reconcile preserves a valid page break unchanged");

    LayoutDocument moved_back_layout = paged_layout;
    expect(moved_back_layout.move_item(35, 0),
           "a last-page item can move back to the first page");
    expect(moved_back_layout.items().size() == 35 &&
               std::ranges::none_of(
                   moved_back_layout.items(),
                   [](const LayoutItem& item) {
                       return item.kind ==
                           LayoutItemKind::page_break;
                   }),
           "moving the only item back removes the orphaned break");

    const std::filesystem::path roundtrip_path =
        test_store_path("roundtrip");
    std::filesystem::remove(roundtrip_path);
    expect(save_layout(roundtrip_path, paged_layout),
           "version two layout can be saved");
    {
        std::ifstream stream(roundtrip_path, std::ios::binary);
        std::string header;
        std::getline(stream, header);
        expect(header == "WindowsLaunchpadLayout/2",
               "saved layouts use the version two header");
    }
    LayoutDocument roundtrip_layout;
    expect(load_layout(roundtrip_path, roundtrip_layout),
           "version two layout can be loaded");
    expect(roundtrip_layout.items() == paged_layout.items(),
           "version two roundtrip preserves explicit page breaks");
    std::filesystem::remove(roundtrip_path);

    const std::filesystem::path legacy_path =
        test_store_path("legacy");
    std::filesystem::remove(legacy_path);
    expect(write_test_store(
               legacy_path,
               "WindowsLaunchpadLayout/1\n"
               "A\t00000078\t00000058\n"),
           "legacy fixture can be written");
    LayoutDocument legacy_layout;
    expect(load_layout(legacy_path, legacy_layout),
           "version one layouts remain readable");
    expect(legacy_layout.items().size() == 1 &&
               legacy_layout.items()[0].app_path == L"x" &&
               legacy_layout.items()[0].name == L"X",
           "legacy application records retain their values");
    std::filesystem::remove(legacy_path);

    const std::filesystem::path normalized_path =
        test_store_path("normalized");
    std::filesystem::remove(normalized_path);
    expect(write_test_store(
               normalized_path,
               "WindowsLaunchpadLayout/2\n"
               "P\n"
               "P\n"
               "A\t00000078\t00000058\n"
               "P\n"
               "P\n"
               "A\t00000079\t00000059\n"
               "P\n"),
           "page normalization fixture can be written");
    LayoutDocument normalized_layout;
    expect(load_layout(normalized_path, normalized_layout),
           "version two page markers can be loaded");
    expect(normalized_layout.items().size() == 3 &&
               normalized_layout.items()[0].app_path == L"x" &&
               normalized_layout.items()[1].kind ==
                   LayoutItemKind::page_break &&
               normalized_layout.items()[2].app_path == L"y",
           "leading, trailing, and consecutive breaks are normalized");
    std::filesystem::remove(normalized_path);

    const std::filesystem::path invalid_v1_path =
        test_store_path("invalid-v1");
    std::filesystem::remove(invalid_v1_path);
    expect(write_test_store(
               invalid_v1_path,
               "WindowsLaunchpadLayout/1\nP\n"),
           "invalid legacy fixture can be written");
    LayoutDocument invalid_v1_layout;
    expect(!load_layout(invalid_v1_path, invalid_v1_layout),
           "page break records are rejected under the version one header");
    std::filesystem::remove(invalid_v1_path);

    LayoutDocument orphaned_layout = paged_layout;
    page_apps.pop_back();
    expect(orphaned_layout.reconcile(page_apps),
           "removing the only page-two app changes the layout");
    expect(orphaned_layout.items().size() == 34 &&
               std::ranges::none_of(
                   orphaned_layout.items(),
                   [](const LayoutItem& item) {
                       return item.kind ==
                           LayoutItemKind::page_break;
                   }),
           "reconcile removes a trailing orphaned page break");

    const std::vector<std::pair<std::wstring, std::wstring>> reduced_apps{
        {L"C:\\Apps\\Alpha.lnk", L"Alpha renamed"},
        {L"C:\\Apps\\Gamma.lnk", L"Gamma"},
    };
    expect(layout.reconcile(reduced_apps),
           "layout reconciles removed applications");
    expect(layout.items()[0].children.size() == 2,
           "missing folder children are removed");

    if (failures == 0) {
        std::cout << "All Launchpad model tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
