#pragma once
#include <string_view>

namespace mh::render {
// Exact, normalized original texture names. Unlisted/mixed facades deliberately
// keep the opaque rough fallback; a substring such as "vag" is not a material.
constexpr bool material_list_contains(std::string_view list, std::string_view name) {
  while (!list.empty()) {
    const auto end = list.find(' ');
    if (list.substr(0, end) == name) return true;
    if (end == std::string_view::npos) break;
    list.remove_prefix(end + 1U);
  }
  return false;
}
constexpr float classify_world_material(std::string_view name, bool textured = true) {
  while (name.starts_with('_')) name.remove_prefix(1U);
  if (name.ends_with(".iff")) name.remove_suffix(4U);
  if (material_list_contains("pcvag1 pcvag1b pcvag1c pcvag1d pvagslo p-falt vbredfz snelinj snelinje "
      "lane lane1_4 lanerand lanestop parklane road1 road2 road3 road4 roadk roadnol roadnor roadnos "
      "roadov roadsat roadsat2 roadstr2 roadstri triroad triroad2 triroad3 triroad4", name)) return 8.0F;
  if (material_list_contains("buske ettree1 ettree2 ettree3 fetree tree1 tree2 tree3 tree5 tree6 tree7 "
      "skog1 skog2 skog3 kenskog1 kenskog2 kenskog3 gras1 gras2 gras3 gras4 grasfa2 grasfad "
      "grasfad1 grasfad2 grasfad3 grasmur grasokk grasokk2 graspa1 graspa2 graspa3", name)) return 10.0F;
  if (material_list_contains("64tberg 64tberg2 mberg1_ v3berg v3berg2 blovagg cybvagg vitvagg vitvagg1 vitvagg3 "
      "brick1 brick1b brick2 brick3 brick4 brocem cemwall newcem1 newcem2 plaster1 "
      "dirt dirtg dirtni~a grsdirt red_dirt reddirt msand1_ msand1_1 msand1_2 sandb sandv sandv2 "
      "nanowall phuswall randwall spawall1", name)) return 11.0F;
  if (material_list_contains("glasdrr glasrail refglas refglas2 lrwin spawin4 sskwin startwin "
      "win2 win3 win4 win5 win6 win7 win7b win8 win9 win10 win10b", name)) return 12.0F;
  if (material_list_contains("lamp1 lamp2 lampa lampa1 brolamp orglamp plamp1 plamp2 utlampa", name)) return 5.0F;
  if (material_list_contains("rmetall rost rostm rostmn bigrost", name)) return 13.0F;
  return textured ? 4.0F : 6.0F;
}
} // namespace mh::render
