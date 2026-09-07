#include <game/vehicle/import.hpp>

#include <content/formats/car_definition.hpp>
#include <content/formats/col_collision_mesh.hpp>
#include <content/formats/motion_path.hpp>
#include <content/formats/myo_object_model.hpp>
#include <content/formats/tbl_envelope.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mh::game {
namespace {

std::string lower_ascii(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

float parse_material_float(const std::string &text,
                           const std::string &field) {
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  float value = 0.0F;
  std::string trailing;
  if (!(input >> value) || (input >> trailing) || !std::isfinite(value) ||
      value <= 0.0F) {
    throw std::invalid_argument("material field " + field +
                                " is not one positive finite float");
  }
  return value;
}

std::uint8_t parse_material_u8(const std::string &text,
                               const std::string &field) {
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  unsigned int value = 0U;
  std::string trailing;
  if (!(input >> value) || (input >> trailing) ||
      value > std::numeric_limits<std::uint8_t>::max()) {
    throw std::invalid_argument("material field " + field +
                                " is not one byte");
  }
  return static_cast<std::uint8_t>(value);
}

OriginalMaterialSoundDefinition
parse_material_sound(const std::string &text, const std::string &field) {
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  OriginalMaterialSoundDefinition result;
  long long parameter_1 = 0;
  long long parameter_2 = 0;
  long long parameter_5 = 0;
  long long parameter_6 = 0;
  std::string trailing;
  if (!(input >> result.wave_file >> parameter_1 >> parameter_2 >>
        result.scalar_1 >> result.scalar_2 >> parameter_5 >> parameter_6) ||
      (input >> trailing) || result.wave_file.empty() ||
      result.wave_file.size() > 255U || !std::isfinite(result.scalar_1) ||
      !std::isfinite(result.scalar_2) ||
      parameter_1 < std::numeric_limits<std::int32_t>::min() ||
      parameter_1 > std::numeric_limits<std::int32_t>::max() ||
      parameter_2 < std::numeric_limits<std::int32_t>::min() ||
      parameter_2 > std::numeric_limits<std::int32_t>::max() ||
      parameter_5 < std::numeric_limits<std::int32_t>::min() ||
      parameter_5 > std::numeric_limits<std::int32_t>::max() ||
      parameter_6 < std::numeric_limits<std::int32_t>::min() ||
      parameter_6 > std::numeric_limits<std::int32_t>::max()) {
    throw std::invalid_argument("material field " + field +
                                " is not one WAV plus six finite parameters");
  }
  result.parameter_1 = static_cast<std::int32_t>(parameter_1);
  result.parameter_2 = static_cast<std::int32_t>(parameter_2);
  result.parameter_5 = static_cast<std::int32_t>(parameter_5);
  result.parameter_6 = static_cast<std::int32_t>(parameter_6);
  return result;
}

std::pair<std::string, std::string> split_material_line(std::string line) {
  const auto comment = line.find("//");
  if (comment != std::string::npos) {
    line.erase(comment);
  }
  std::istringstream input(line);
  std::string field;
  if (!(input >> field)) {
    return {};
  }
  std::string value;
  std::getline(input, value);
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string::npos) {
    value.clear();
  } else {
    value.erase(0U, first);
    const auto last = value.find_last_not_of(" \t\r");
    value.erase(last + 1U);
  }
  return {lower_ascii(std::move(field)), std::move(value)};
}

template <typename Record>
void validate_environment_sound_record(const Record &record,
                                       const std::size_t line_number) {
  if (record.wave_file.empty() || record.wave_file.size() > 255U ||
      !std::isfinite(record.volume) || !std::isfinite(record.pitch) ||
      record.volume < 0.0F || record.pitch < 0.0F) {
    throw std::invalid_argument(
        "environment sound record on line " + std::to_string(line_number) +
        " has an invalid WAV, volume, or pitch");
  }
}

void validate_environment_loop_distances(const float minimum_distance,
                                         const float maximum_distance,
                                         const std::size_t line_number) {
  if (!std::isfinite(minimum_distance) ||
      !std::isfinite(maximum_distance) || minimum_distance < 0.0F ||
      maximum_distance <= minimum_distance) {
    throw std::invalid_argument(
        "environment sound record on line " + std::to_string(line_number) +
        " has invalid distance bounds");
  }
}

std::unordered_map<std::string, OriginalVehicleGroundedMaterialProfile>
read_global_grounded_materials(const std::filesystem::path &path) {
  const auto envelope = mh::content::read_tbl(path, "EQ", 1024ULL * 1024ULL,
                                               1024ULL * 1024ULL);
  if (!envelope.decoded) {
    throw std::invalid_argument("Material.mat could not be decoded: " +
                                envelope.decode_status);
  }
  const std::string text(envelope.payload.begin(), envelope.payload.end());
  std::istringstream lines(text);
  std::unordered_map<std::string, OriginalVehicleGroundedMaterialProfile>
      result;
  std::string current_name;
  OriginalVehicleGroundedMaterialProfile current;
  const auto commit = [&]() {
    if (!current_name.empty()) {
      if (!result.emplace(current_name, current).second) {
        throw std::invalid_argument("duplicate Material.mat material: " +
                                    current_name);
      }
    }
  };
  std::string line;
  while (std::getline(lines, line)) {
    const auto [field, value] = split_material_line(std::move(line));
    if (field.empty()) {
      continue;
    }
    if (field == "materialname") {
      commit();
      current_name = lower_ascii(value);
      current = {};
      continue;
    }
    if (current_name.empty()) {
      continue;
    }
    if (field == "spinspeedlimit") {
      current.lateral_threshold = parse_material_float(value, field);
    } else if (field == "spinrotlimit") {
      current.yaw_threshold = parse_material_float(value, field);
    } else if (field == "xspinfriction") {
      current.alternate_lateral_base = parse_material_float(value, field);
    } else if (field == "xnospinfriction") {
      current.lateral_base = parse_material_float(value, field);
    } else if (field == "zspinfriction") {
      current.alternate_longitudinal_base =
          parse_material_float(value, field);
    } else if (field == "znospinfriction") {
      current.longitudinal_base = parse_material_float(value, field);
    } else if (field == "rotspinfriction") {
      current.alternate_angular_y_base =
          parse_material_float(value, field);
    } else if (field == "rotnospinfriction") {
      current.angular_y_base = parse_material_float(value, field);
    } else if (field == "spinturnreduce") {
      current.spin_turn_reduce = parse_material_float(value, field);
    } else if (field == "nospinturnreduce") {
      current.no_spin_turn_reduce = parse_material_float(value, field);
    } else if (field == "sndcontact") {
      current.contact_sound = parse_material_sound(value, field);
    } else if (field == "sndweakimpulse") {
      current.weak_impulse_sound = parse_material_sound(value, field);
    } else if (field == "sndhardimpulse") {
      current.hard_impulse_sound = parse_material_sound(value, field);
    } else if (field == "sndscratch") {
      current.scratch_sound = parse_material_sound(value, field);
    } else if (field == "sndslide") {
      current.slide_sound = parse_material_sound(value, field);
    } else if (field == "sndspin") {
      current.spin_sound = parse_material_sound(value, field);
    } else if (field == "sparkanimid") {
      current.spark_animation_id = parse_material_u8(value, field);
    } else if (field == "slidetype") {
      current.slide_type = parse_material_u8(value, field);
    } else if (field == "skidmark") {
      current.skid_mark = parse_material_u8(value, field);
    }
  }
  commit();
  if (result.empty()) {
    throw std::invalid_argument("Material.mat contains no material rows");
  }
  return result;
}

std::vector<std::string>
read_track_material_names(const std::filesystem::path &path) {
  constexpr std::uintmax_t maximum_bytes = 1024ULL * 1024ULL;
  const auto status = std::filesystem::symlink_status(path);
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status) ||
      std::filesystem::file_size(path) > maximum_bytes) {
    throw std::invalid_argument(
        "track surface definition is not a bounded plain file");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::invalid_argument("cannot open track surface definition");
  }
  std::string text(static_cast<std::size_t>(std::filesystem::file_size(path)),
                   '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (input.gcount() != static_cast<std::streamsize>(text.size())) {
    throw std::invalid_argument("short read from track surface definition");
  }
  if (text.size() >= 3U && text[0U] == 'T' && text[1U] == 'B' &&
      text[2U] == 'L') {
    const auto envelope =
        mh::content::read_tbl(path, "EQ", maximum_bytes, maximum_bytes);
    if (!envelope.decoded) {
      throw std::invalid_argument(
          "track surface definition could not be decoded: " +
          envelope.decode_status);
    }
    text.assign(envelope.payload.begin(), envelope.payload.end());
  }
  std::istringstream lines(text);
  std::vector<std::string> result;
  std::string line;
  while (std::getline(lines, line)) {
    const auto [field, value] = split_material_line(std::move(line));
    if (field == "material") {
      result.push_back(lower_ascii(value));
    }
  }
  if (result.empty() ||
      result.size() > OriginalVehicleGroundedMaterialTable{}.size()) {
    throw std::invalid_argument(
        "track surface definition has an invalid material count");
  }
  return result;
}

} // namespace

OriginalEnvironmentSoundTable
parse_original_environment_sounds(const std::string_view text) {
  constexpr std::size_t maximum_bytes = 1024U * 1024U;
  if (text.empty() || text.size() > maximum_bytes ||
      text.find('\0') != std::string_view::npos) {
    throw std::invalid_argument(
        "environment sound text is empty, oversized, or contains NUL");
  }

  OriginalEnvironmentSoundTable result;
  std::istringstream lines{std::string(text)};
  lines.imbue(std::locale::classic());
  std::string line;
  std::size_t line_number = 0U;
  while (std::getline(lines, line)) {
    ++line_number;
    if (line.size() > 4096U) {
      throw std::invalid_argument("environment sound line exceeds 4096 bytes");
    }
    const auto comment = line.find("//");
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    std::istringstream fields(line);
    fields.imbue(std::locale::classic());
    std::string kind;
    if (!(fields >> kind)) {
      continue;
    }
    kind = lower_ascii(std::move(kind));
    std::string trailing;
    if (kind == "envsound") {
      OriginalEnvironmentLoopSound record;
      if (!(fields >> record.wave_file >> record.priority >> record.volume >>
            record.pitch >> record.minimum_distance >>
            record.maximum_distance >> record.position[0U] >>
            record.position[1U] >> record.position[2U]) ||
          (fields >> trailing) ||
          std::any_of(record.position.begin(), record.position.end(),
                      [](const auto value) { return !std::isfinite(value); })) {
        throw std::invalid_argument("malformed EnvSound on line " +
                                    std::to_string(line_number));
      }
      validate_environment_sound_record(record, line_number);
      validate_environment_loop_distances(record.minimum_distance,
                                          record.maximum_distance,
                                          line_number);
      result.fixed_loops.push_back(std::move(record));
    } else if (kind == "objectenvsound") {
      OriginalObjectEnvironmentLoopSound record;
      if (!(fields >> record.object_number >> record.wave_file >>
            record.priority >> record.volume >> record.pitch >>
            record.minimum_distance >> record.maximum_distance) ||
          (fields >> trailing)) {
        throw std::invalid_argument("malformed ObjectEnvSound on line " +
                                    std::to_string(line_number));
      }
      validate_environment_sound_record(record, line_number);
      validate_environment_loop_distances(record.minimum_distance,
                                          record.maximum_distance,
                                          line_number);
      result.object_loops.push_back(std::move(record));
    } else if (kind == "objectcolsound") {
      OriginalObjectCollisionSound record;
      if (!(fields >> record.object_number >> record.wave_file >>
            record.priority >> record.volume >> record.pitch) ||
          (fields >> trailing)) {
        throw std::invalid_argument("malformed ObjectColSound on line " +
                                    std::to_string(line_number));
      }
      validate_environment_sound_record(record, line_number);
      result.object_collisions.push_back(std::move(record));
    }
  }
  return result;
}

OriginalEnvironmentSoundTable
read_original_environment_sounds(const std::filesystem::path &path) {
  constexpr std::uintmax_t maximum_bytes = 1024ULL * 1024ULL;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw std::invalid_argument(
        "environment sound definition is not a plain file");
  }
  const auto byte_count = std::filesystem::file_size(path, error);
  if (error || byte_count == 0U || byte_count > maximum_bytes) {
    throw std::invalid_argument(
        "environment sound definition is empty or oversized");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::invalid_argument("cannot open environment sound definition");
  }
  std::string text(static_cast<std::size_t>(byte_count), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (input.gcount() != static_cast<std::streamsize>(text.size())) {
    throw std::invalid_argument("short read from environment sound definition");
  }
  return parse_original_environment_sounds(text);
}

OriginalEnvironmentScene
read_original_environment_scene(const std::filesystem::path &path) {
  constexpr std::uint64_t maximum_bytes = 16ULL * 1024ULL * 1024ULL;
  const auto envelope =
      mh::content::read_tbl(path, "EQ", maximum_bytes, maximum_bytes);
  if (!envelope.decoded) {
    throw std::invalid_argument("environment scene could not be decoded: " +
                                envelope.decode_status);
  }
  const std::string text(envelope.payload.begin(), envelope.payload.end());
  std::istringstream lines(text);
  lines.imbue(std::locale::classic());
  OriginalEnvironmentScene result;
  OriginalEnvironmentSceneObject *current = nullptr;
  std::string line;
  while (std::getline(lines, line)) {
    if (line.size() > 4096U) {
      throw std::invalid_argument("environment scene line exceeds 4096 bytes");
    }
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
      continue;
    }
    const auto last = line.find_last_not_of(" \t\r");
    const auto trimmed = line.substr(first, last - first + 1U);
    std::istringstream fields(trimmed);
    fields.imbue(std::locale::classic());
    std::string field;
    fields >> field;
    const auto lowered = lower_ascii(field);
    if (lowered == "framespersecond") {
      std::string trailing;
      if (!(fields >> result.frames_per_second) || (fields >> trailing) ||
          !std::isfinite(result.frames_per_second) ||
          result.frames_per_second <= 0.0) {
        throw std::invalid_argument(
            "environment scene has invalid FramesPerSecond");
      }
    } else if (lowered == "loadobject") {
      std::string source;
      std::getline(fields, source);
      const auto source_first = source.find_first_not_of(" \t");
      if (source_first == std::string::npos) {
        throw std::invalid_argument("environment scene LoadObject is empty");
      }
      result.objects.push_back({});
      current = &result.objects.back();
      current->source_object = source.substr(source_first);
    } else if (lowered == "objectmotion" && current != nullptr) {
      std::string value_count_line;
      std::string key_count_line;
      if (!std::getline(lines, value_count_line) ||
          !std::getline(lines, key_count_line)) {
        throw std::invalid_argument(
            "environment scene ObjectMotion header is truncated");
      }
      std::istringstream value_count_stream(value_count_line);
      std::istringstream key_count_stream(key_count_line);
      std::size_t value_count = 0U;
      std::size_t key_count = 0U;
      std::string trailing;
      if (!(value_count_stream >> value_count) ||
          (value_count_stream >> trailing) || value_count != 9U ||
          !(key_count_stream >> key_count) ||
          (key_count_stream >> trailing) || key_count == 0U ||
          key_count > 100000U) {
        throw std::invalid_argument(
            "environment scene ObjectMotion header is invalid");
      }
      current->keys.reserve(key_count);
      for (std::size_t key_index = 0U; key_index < key_count; ++key_index) {
        std::string values_line;
        std::string controls_line;
        if (!std::getline(lines, values_line) ||
            !std::getline(lines, controls_line)) {
          throw std::invalid_argument(
              "environment scene ObjectMotion key is truncated");
        }
        std::istringstream values(values_line);
        std::istringstream controls(controls_line);
        values.imbue(std::locale::classic());
        controls.imbue(std::locale::classic());
        OriginalEnvironmentScenePositionKey key;
        if (!(values >> key.position[0U] >> key.position[1U] >>
              key.position[2U] >> key.rotation[0U] >> key.rotation[1U] >>
              key.rotation[2U] >> key.attributes[0U] >> key.attributes[1U] >>
              key.attributes[2U])) {
          throw std::invalid_argument(
              "environment scene ObjectMotion value row is incomplete");
        }
        if ((values >> trailing) ||
            !(controls >> key.frame >> key.linear >> key.tension >>
              key.continuity >> key.bias) ||
            (controls >> trailing) ||
            std::any_of(key.position.begin(), key.position.end(),
                        [](const auto value) { return !std::isfinite(value); }) ||
            std::any_of(key.rotation.begin(), key.rotation.end(),
                        [](const auto value) { return !std::isfinite(value); }) ||
            std::any_of(key.attributes.begin(), key.attributes.end(),
                        [](const auto value) { return !std::isfinite(value); }) ||
            !std::isfinite(key.tension) || !std::isfinite(key.continuity) ||
            !std::isfinite(key.bias)) {
          throw std::invalid_argument(
              "environment scene ObjectMotion key is invalid");
        }
        current->keys.push_back(key);
      }
    } else if (lowered == "endbehavior" && current != nullptr) {
      std::string trailing;
      if (!(fields >> current->end_behavior) || (fields >> trailing) ||
          (current->end_behavior != 1 && current->end_behavior != 2)) {
        throw std::invalid_argument(
            "environment scene EndBehavior is unsupported");
      }
    } else if (lowered == "objedgecolor" && current != nullptr) {
      long long identifier = 0;
      long long collision_identifier = 0;
      long long ignored_blue = 0;
      std::string trailing;
      if (!(fields >> identifier >> collision_identifier >> ignored_blue) ||
          (fields >> trailing) || identifier < 0 || identifier > 65535 ||
          collision_identifier < 0 || collision_identifier > 65535) {
        throw std::invalid_argument(
            "environment scene ObjEdgeColor is invalid");
      }
      current->environment_identifier =
          static_cast<std::uint16_t>(identifier);
      current->collision_identifier =
          static_cast<std::uint16_t>(collision_identifier);
    } else if (lowered == "polygonsize" && current != nullptr) {
      std::string trailing;
      double polygon_size = 0.0;
      if (!(fields >> polygon_size) || (fields >> trailing) ||
          !std::isfinite(polygon_size) || polygon_size < 0.0) {
        throw std::invalid_argument(
            "environment scene PolygonSize is invalid");
      }
      current->polygon_size = polygon_size;
    } else if (lowered == "unseenbyrays" && current != nullptr) {
      std::int32_t value = 0;
      std::string trailing;
      if (!(fields >> value) || (fields >> trailing) ||
          (value != 0 && value != 1)) {
        throw std::invalid_argument(
            "environment scene UnseenByRays is invalid");
      }
      current->unseen_by_rays = value != 0;
    }
  }
  result.objects.erase(
      std::remove_if(result.objects.begin(), result.objects.end(),
                     [](const auto &object) { return object.keys.empty(); }),
      result.objects.end());
  if (result.objects.empty()) {
    throw std::invalid_argument("environment scene has no object motions");
  }
  return result;
}

std::array<double, 3U> sample_original_environment_scene_position(
    const OriginalEnvironmentSceneObject &object,
    double authored_frame) {
  return sample_original_environment_scene_motion(object, authored_frame)
      .position;
}

mh::content::MotionSample sample_original_environment_scene_motion(
    const OriginalEnvironmentSceneObject &object,
    double authored_frame) {
  if (object.keys.empty() || !std::isfinite(authored_frame)) {
    throw std::invalid_argument(
        "environment scene sample requires keys and a finite frame");
  }
  const auto first_frame = object.keys.front().frame;
  const auto last_frame = object.keys.back().frame;
  if (object.end_behavior == 2 && last_frame > first_frame &&
      authored_frame > static_cast<double>(last_frame)) {
    const auto span = static_cast<double>(last_frame - first_frame);
    authored_frame = static_cast<double>(first_frame) +
                     std::fmod(authored_frame -
                                   static_cast<double>(first_frame),
                               span);
  }
  mh::content::MotionData motion;
  motion.version = 1U;
  motion.value_count = 9U;
  motion.keyframes.reserve(object.keys.size());
  for (const auto &source : object.keys) {
    mh::content::MotionKeyframe key;
    key.position = source.position;
    key.rotation = source.rotation;
    key.attributes = source.attributes;
    key.frame = source.frame;
    key.linear = source.linear;
    key.tension = source.tension;
    key.continuity = source.continuity;
    key.bias = source.bias;
    motion.keyframes.push_back(key);
  }
  return mh::content::motion_sample(motion, authored_frame);
}

bool original_environment_scene_has_transform_motion(
    const OriginalEnvironmentSceneObject &object) noexcept {
  if (object.keys.size() < 2U) {
    return false;
  }
  const auto &first = object.keys.front();
  return std::any_of(
      object.keys.begin() + 1, object.keys.end(), [&](const auto &key) {
        return key.position != first.position || key.rotation != first.rotation;
      });
}

OriginalBodyPoseState make_original_environment_collision_pose(
    const OriginalEnvironmentSceneObject &object,
    const double authored_frame) {
  const auto sample =
      sample_original_environment_scene_motion(object, authored_frame);
  // The original conversion uses the binary64 constants 3.1415926 and
  // 1/180, then stores the matrix and translation as binary32.
  constexpr double original_pi = 3.1415926;
  constexpr double degrees_to_radians = original_pi / 180.0;
  const auto angle_a = sample.rotation[1U] * degrees_to_radians;
  const auto angle_b = sample.rotation[0U] * degrees_to_radians;
  const auto angle_c = sample.rotation[2U] * degrees_to_radians;
  const auto sine_a = std::sin(angle_a);
  const auto cosine_a = std::cos(angle_a);
  const auto sine_b = std::sin(angle_b);
  const auto cosine_b = std::cos(angle_b);
  const auto sine_c = std::sin(angle_c);
  const auto cosine_c = std::cos(angle_c);
  const auto stored = [](const double value) {
    return static_cast<double>(static_cast<float>(value));
  };

  OriginalBodyPoseState pose;
  // Exact output layout of matrix helper RVA 0x00043d1c.
  pose.body_basis = {{
      {stored(sine_c * sine_a * sine_b + cosine_c * cosine_b),
       stored(sine_c * cosine_a),
       stored(sine_c * sine_a * cosine_b - cosine_c * sine_b)},
      {stored(cosine_c * sine_a * sine_b - sine_c * cosine_b),
       stored(cosine_c * cosine_a),
       stored(cosine_c * sine_a * cosine_b + sine_c * sine_b)},
      {stored(cosine_a * sine_b), stored(-sine_a),
       stored(cosine_a * cosine_b)},
  }};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    pose.world_position[axis] = stored(sample.position[axis]);
  }
  return pose;
}

OriginalDynamicVehicleContactShape make_original_environment_collision_shape(
    const mh::content::MyoData &model,
    const mh::content::ColData &collision,
    const std::optional<double> polygon_size) {
  if (model.positions.empty()) {
    throw std::invalid_argument(
        "environment MYO omits collision-body bounds");
  }
  if (collision.faces.empty()) {
    throw std::invalid_argument(
        "environment COL omits convex face planes");
  }
  if (polygon_size.has_value() &&
      (!std::isfinite(*polygon_size) || *polygon_size < 0.0)) {
    throw std::invalid_argument(
        "environment PolygonSize must be finite and nonnegative");
  }

  OriginalDynamicVehicleContactShape shape;
  std::array<double, 3U> minimum{};
  std::array<double, 3U> maximum{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    minimum[axis] = static_cast<double>(model.positions.front()[axis]);
    maximum[axis] = minimum[axis];
  }
  for (const auto &position : model.positions) {
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      const auto value = static_cast<double>(position[axis]);
      if (!std::isfinite(value)) {
        throw std::invalid_argument(
            "environment MYO has a non-finite position");
      }
      minimum[axis] = std::min(minimum[axis], value);
      maximum[axis] = std::max(maximum[axis], value);
    }
  }

  shape.local_points.reserve(8U);
  for (std::size_t corner = 0U; corner < 8U; ++corner) {
    // RVA 0x0007109d..0x000710f6 chooses maximum for a clear bit and
    // minimum for a set bit, in X/Y/Z bit order.
    shape.local_points.push_back(
        {(corner & 1U) == 0U ? maximum[0U] : minimum[0U],
         (corner & 2U) == 0U ? maximum[1U] : minimum[1U],
         (corner & 4U) == 0U ? maximum[2U] : minimum[2U]});
  }
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    shape.local_center_of_mass[axis] =
        (minimum[axis] + maximum[axis]) * 0.5;
  }

  const std::array<double, 3U> maximum_absolute{
      std::max(std::fabs(minimum[0U]), std::fabs(maximum[0U])),
      std::max(std::fabs(minimum[1U]), std::fabs(maximum[1U])),
      std::max(std::fabs(minimum[2U]), std::fabs(maximum[2U]))};
  shape.bounding_radius =
      std::sqrt(maximum_absolute[0U] * maximum_absolute[0U] +
                maximum_absolute[1U] * maximum_absolute[1U] +
                maximum_absolute[2U] * maximum_absolute[2U]);
  shape.half_width = maximum_absolute[0U];
  shape.half_length = maximum_absolute[2U];
  if (!std::isfinite(shape.bounding_radius) ||
      shape.bounding_radius <= 0.0) {
    throw std::invalid_argument("environment MYO bounds are degenerate");
  }

  // PolygonSize is multiplied by 100 at RVA 0x00070c07..0x00070c12.
  // A zero/absent value takes RVA 0x00070f0d..0x00070f22:
  // radius^3 * 12566.370616 / 3 (4*pi*1000/3).
  constexpr double original_polygon_size_mass_scale = 100.0;
  constexpr double original_sphere_mass_numerator = 12566.370616;
  constexpr double original_one_third = 1.0 / 3.0;
  const auto mass = polygon_size.has_value() && *polygon_size != 0.0
                        ? *polygon_size * original_polygon_size_mass_scale
                        : shape.bounding_radius * shape.bounding_radius *
                              shape.bounding_radius *
                              original_sphere_mass_numerator *
                              original_one_third;
  if (!std::isfinite(mass) || mass <= 0.0) {
    throw std::invalid_argument(
        "environment collision-body mass is invalid");
  }
  shape.mass_properties.mass = mass;
  const std::array<double, 3U> extent{
      maximum[0U] - minimum[0U], maximum[1U] - minimum[1U],
      maximum[2U] - minimum[2U]};
  constexpr double original_box_inertia_scale = 1.0 / 12.0;
  shape.mass_properties.principal_inertia = {
      mass * original_box_inertia_scale *
          (extent[1U] * extent[1U] + extent[2U] * extent[2U]),
      mass * original_box_inertia_scale *
          (extent[0U] * extent[0U] + extent[2U] * extent[2U]),
      mass * original_box_inertia_scale *
          (extent[0U] * extent[0U] + extent[1U] * extent[1U])};
  if (std::any_of(shape.mass_properties.principal_inertia.begin(),
                  shape.mass_properties.principal_inertia.end(),
                  [](const double value) {
                    return !std::isfinite(value) || value <= 0.0;
                  })) {
    throw std::invalid_argument(
        "environment collision-body inertia is invalid");
  }

  shape.local_planes.reserve(collision.faces.size());
  for (const auto &face : collision.faces) {
    shape.local_planes.push_back(
        {{static_cast<double>(face.normal[0U]),
          static_cast<double>(face.normal[1U]),
          static_cast<double>(face.normal[2U])},
         static_cast<double>(face.distance), face.surface});
  }
  return shape;
}

WheelProbeRig make_wheel_probe_rig(const mh::content::CarDefinition &car,
                                   const double query_distance) {
  if (!car.physics.has_value()) {
    throw std::invalid_argument("CAR definition omits wheel-physics data");
  }
  WheelProbeRig rig;
  for (std::size_t index = 0U; index < rig.local_origins.size(); ++index) {
    const auto &center = car.wheels[index].center;
    rig.local_origins[index] = {static_cast<double>(center.x),
                                static_cast<double>(center.y),
                                static_cast<double>(center.z)};
  }
  rig.query_distance = query_distance;
  rig.authored_spring_length = car.physics->spring_length;
  rig.authored_spring_strength = car.physics->spring_strength;
  return rig;
}

OriginalVehicleResponseConfig make_original_vehicle_response_config(
    const mh::content::CarDefinition &car, const double query_distance,
    const OriginalWheelResponseProfile profile) {
  if (!car.physics.has_value()) {
    throw std::invalid_argument("CAR definition omits body-response data");
  }
  return {make_wheel_probe_rig(car, query_distance),
          {static_cast<double>(car.center_of_mass.x),
           static_cast<double>(car.center_of_mass.y),
           static_cast<double>(car.center_of_mass.z)},
          make_original_body_mass_properties(car.physics->weight),
          profile};
}

OriginalVehicleGroundedMaterialTable
make_original_track_grounded_material_table(
    const std::filesystem::path &material_definition,
    const std::filesystem::path &track_surface_definition) {
  const auto global = read_global_grounded_materials(material_definition);
  const auto local = read_track_material_names(track_surface_definition);
  OriginalVehicleGroundedMaterialTable result{};
  for (std::size_t index = 0U; index < local.size(); ++index) {
    // Several shipping SRFs use the literal sentinel "default"; p3.1 leaves
    // that local slot on the initializer row rather than resolving a named
    // Material.mat override.
    if (local[index] == "default") {
      continue;
    }
    const auto found = global.find(local[index]);
    if (found == global.end()) {
      throw std::invalid_argument("track surface material is absent from "
                                  "Material.mat: " +
                                  local[index]);
    }
    result[index] = found->second;
  }
  return result;
}

BodyHullRig
make_original_vehicle_body_hull_rig(const mh::content::CarDefinition &car) {
  if (!car.physics.has_value()) {
    throw std::invalid_argument("CAR definition omits body-corner data");
  }
  BodyHullRig rig;
  rig.local_points.reserve(car.physics->corners.size());
  constexpr std::array<std::size_t, 4U> original_order{0U, 1U, 3U, 2U};
  for (const auto index : original_order) {
    const auto &corner = car.physics->corners[index];
    rig.local_points.push_back({static_cast<double>(corner.x),
                                static_cast<double>(car.center_of_mass.y),
                                static_cast<double>(corner.z)});
  }
  return rig;
}

OriginalDynamicVehicleContactShape make_original_dynamic_vehicle_contact_shape(
    const mh::content::CarDefinition &car,
    const mh::content::ColData &collision) {
  if (!car.physics.has_value()) {
    throw std::invalid_argument("CAR definition omits dynamic-contact data");
  }
  if (collision.faces.empty()) {
    throw std::invalid_argument(
        "CAR collision definition omits convex face planes");
  }
  OriginalDynamicVehicleContactShape shape;
  shape.local_center_of_mass = {
      static_cast<double>(car.center_of_mass.x),
      static_cast<double>(car.center_of_mass.y),
      static_cast<double>(car.center_of_mass.z),
  };
  shape.mass_properties =
      make_original_body_mass_properties(car.physics->weight);
  constexpr std::array<std::size_t, 4U> original_order{0U, 1U, 3U, 2U};
  shape.local_points.reserve(original_order.size());
  for (const auto index : original_order) {
    const auto &corner = car.physics->corners[index];
    shape.local_points.push_back({static_cast<double>(corner.x),
                                  static_cast<double>(car.center_of_mass.y),
                                  static_cast<double>(corner.z)});
    shape.half_width =
        std::max(shape.half_width, std::fabs(static_cast<double>(corner.x)));
    shape.half_length =
        std::max(shape.half_length, std::fabs(static_cast<double>(corner.z)));
    shape.bounding_radius = std::max(shape.bounding_radius,
                                     std::hypot(static_cast<double>(corner.x),
                                                static_cast<double>(corner.z)));
  }
  shape.local_planes.reserve(collision.faces.size());
  for (const auto &face : collision.faces) {
    shape.local_planes.push_back({{static_cast<double>(face.normal[0U]),
                                   static_cast<double>(face.normal[1U]),
                                   static_cast<double>(face.normal[2U])},
                                  static_cast<double>(face.distance),
                                  face.surface});
  }
  if (shape.half_width <= 0.0 || shape.half_length <= 0.0 ||
      shape.bounding_radius <= 0.0) {
    throw std::invalid_argument("CAR dynamic-contact corners are invalid");
  }
  return shape;
}

RecoveredVehicleRuntimeTuning
make_recovered_vehicle_runtime_tuning(const mh::content::CarDefinition &car) {
  if (!car.physics.has_value() || !car.performance_levels.has_value()) {
    throw std::invalid_argument(
        "CAR definition omits drivetrain or performance data");
  }
  const auto &physics = *car.physics;
  const auto &levels = *car.performance_levels;
  return make_recovered_vehicle_runtime_tuning(
      {physics.gear_ratios, physics.minimum_rpm, physics.maximum_rpm,
       physics.acceleration_force, physics.brake_force, physics.turn_force,
       levels.speed, levels.acceleration, levels.grip});
}

} // namespace mh::game
