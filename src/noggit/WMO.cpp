// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/frustum.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/Log.h> // LogDebug
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMO.h>
#include <noggit/World.h>
#include <noggit/rendering/Primitives.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <opengl/scoped.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>


WMO::WMO(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
  : AsyncObject(file_key)
  , _context(context)
  , _renderer(this)
{
}

void WMO::finishLoading ()
{
  BlizzardArchive::ClientFile f(_file_key.filepath(), Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof()) {
    LogError << "Error loading WMO \"" << _file_key.stringRepr() << "\"." << std::endl;
    return;
  }

  uint32_t fourcc;
  uint32_t size;

  float ff[3];

  char const* ddnames = nullptr;
  char const* groupnames = nullptr;

  // - MVER ----------------------------------------------

  uint32_t version;

  f.read (&fourcc, 4);
  f.seekRelative (4);
  f.read (&version, 4);

  assert (fourcc == 'MVER' && version == 17);

  // - MOHD ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  assert (fourcc == 'MOHD');

  CArgb ambient_color;
  unsigned int nTextures, nGroups, nP, nLights, nModels, nDoodads, nDoodadSets;
  // header
  f.read (&nTextures, 4);
  f.read (&nGroups, 4);
  f.read (&nP, 4);
  f.read (&nLights, 4);
  f.read (&nModels, 4);
  f.read (&nDoodads, 4);
  f.read (&nDoodadSets, 4);
  f.read (&ambient_color, 4);
  f.read (&WmoId, 4);
  f.read (ff, 12);
  extents[0] = ::glm::vec3 (ff[0], ff[1], ff[2]);
  f.read (ff, 12);
  extents[1] = ::glm::vec3 (ff[0], ff[1], ff[2]);
  f.read(&flags, 2);

  f.seekRelative (2);

  ambient_light_color.x = static_cast<float>(ambient_color.r) / 255.f;
  ambient_light_color.y = static_cast<float>(ambient_color.g) / 255.f;
  ambient_light_color.z = static_cast<float>(ambient_color.b) / 255.f;
  ambient_light_color.w = static_cast<float>(ambient_color.a) / 255.f;

  // - MOTX ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOTX');

  std::vector<char> texbuf (size);
  f.read (texbuf.data(), texbuf.size());

  // - MOMT ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOMT');

  std::size_t const num_materials (size / 0x40);
  materials.resize (num_materials);

  // note: used to map to size_t, but our other values don't support that.
  //std::map<std::uint32_t, std::size_t> texture_offset_to_inmem_index;
  std::map<std::uint32_t, std::uint32_t> texture_offset_to_inmem_index;

  auto load_texture
    ( [&] (std::uint32_t ofs)
      {
        std::string texture
          (texbuf[ofs] ? std::string(&texbuf[ofs]) : std::string("textures/shanecube.blp"));

        // Custom WMOs (Turtle world/custom/kttown/kttown.wmo) reference textures by BARE filename
        // (window.blp, floor.blp, wall3.blp) with NO directory. A bare name collides with same-named
        // root textures shipped by other patches, so noggit's patch load-order resolves them to the WRONG
        // image ("wrong textures" on the building). Resolve a directory-less name from the WMO's OWN folder
        // first (world/custom/kttown/window.blp) -- a unique, collision-free path -- and only fall back to
        // the bare name if no co-located texture exists. Standard full-path MOTX entries are unaffected.
        if (texture.find('/') == std::string::npos && texture.find('\\') == std::string::npos)
        {
          std::string const wmo_path = _file_key.filepath();
          auto const slash = wmo_path.find_last_of("/\\");
          if (slash != std::string::npos)
          {
            std::string const co_located = wmo_path.substr(0, slash + 1) + texture;
            if (Noggit::Application::NoggitApplication::instance()->clientData()->exists(co_located))
            {
              texture = co_located;
            }
          }
        }

        auto const mapping
          (texture_offset_to_inmem_index.emplace(ofs, static_cast<std::uint32_t>(textures.size())));

        if (mapping.second)
        {
          textures.emplace_back(texture, _context);
        }
        return mapping.first->second;
      }
    );

  for (size_t i(0); i < num_materials; ++i)
  {
    f.read(&materials[i], sizeof(WMOMaterial));

    uint32_t shader = materials[i].shader;
    bool use_second_texture = (shader == 6 || shader == 5 || shader == 3);

    materials[i].texture1 = load_texture(materials[i].texture_offset_1);
    if (use_second_texture)
    {
      materials[i].texture2 = load_texture(materials[i].texture_offset_2);
    }
  }

  // - MOGN ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOGN');

  groupnames = reinterpret_cast<char const*> (f.getPointer ());

  f.seekRelative (size);

  // - MOGI ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOGI');

  groups.reserve(nGroups);
  for (int i (0); i < nGroups; ++i) {
    groups.emplace_back (this, &f, i, groupnames);
  }

  // - MOSB ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOSB');

  if (size > 4)
  {
    std::string path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::string (reinterpret_cast<char const*>(f.getPointer ())));
    auto from = std::string("mdx");
    auto to = std::string("m2");
    size_t start_pos = 0;
    while ((start_pos = path.find(from, start_pos)) != std::string::npos) {
        path.replace(start_pos, from.length(), to);
        start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
    }

    if (path.length())
    {
      if (Noggit::Application::NoggitApplication::instance()->clientData()->exists(path))
      {
        skybox = scoped_model_reference(path, _context);
      }
    }
  }

  f.seekRelative (size);

  // - MOPV ----------------------------------------------

  f.read (&fourcc, 4);
  f.read(&size, 4);

  assert (fourcc == 'MOPV');

  // Portal polygon corners. Same X/Z-up -> Y-up swap the rest of the WMO geometry uses.
  _portal_vertices.reserve(size / 12);
  for (size_t i (0); i < size / 12; ++i)
  {
    f.read (ff, 12);
    _portal_vertices.push_back(glm::vec3(ff[0], ff[2], -ff[1]));
  }

  // - MOPT ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOPT');

  // Each MOPT is uint16 base_index, uint16 count, then a 16-byte C4Plane. The plane is STORED (in the
  // swapped coord convention, an orthogonal transform, so distances are invariant) -- AttenTransVerts
  // (RE_notes/19) needs the authored plane for its on-plane test. Culling still recomputes its own.
  _portal_info.reserve(size / 0x14);
  for (size_t i (0); i < size / 0x14; ++i)
  {
    wmo_portal_info info;
    f.read (&info.base_vertex, 2);
    f.read (&info.vertex_count, 2);
    float plane[4];
    f.read (plane, 16);
    info.plane_normal = glm::vec3(plane[0], plane[2], -plane[1]); // same swap as MOPV/MOVT
    info.plane_dist = plane[3];
    _portal_info.push_back(info);
  }

  // - MOPR ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert(fourcc == 'MOPR');

  _portal_refs.resize(size / sizeof(WMOPR));
  if (size)
  {
    f.read (_portal_refs.data(), size);
  }

  // - MOVV ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVV');

  f.seekRelative (size);

  // - MOVB ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVB');

  f.seekRelative (size);

  // - MOLT ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOLT');

  // Same header-vs-chunk-size guard as MODS below: trust the CHUNK SIZE so a bad MOHD.nLights can't run
  // the read past the chunk and desync the stream. Each MOLT entry is 0x30 bytes; min() is a no-op when
  // the header agrees with the chunk (every valid WMO), and seeking to the chunk end keeps alignment.
  std::size_t const molt_end = f.getPos () + size;
  std::size_t const light_count = std::min<std::size_t> (nLights, size / 0x30);
  lights.reserve(light_count);
  for (size_t i (0); i < light_count; ++i) {
    WMOLight l;
    l.init (&f);
    lights.push_back (l);
  }
  f.seek (molt_end);

  // - MODS ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MODS');

  // Robustness (Turtle world/custom/kt_Farm/ktfarm.wmo, kt_Inn/ktinn.wmo): read the doodad-set count from
  // the CHUNK SIZE, not blindly from MOHD.nDoodadSets. Those custom WMOs carry a MOHD nDoodadSets (6, 4)
  // that OVERRUNS their actual 32-byte (1-set) MODS chunk -> reading nDoodadSets*32 bytes ran past the
  // chunk and DESYNCED every following chunk (MODN/MODD/MFOG read from garbage offsets -> MODD "claimed"
  // 34.5M doodads -> async loader crash, SEH 0xC0000005). Clamp to what the chunk holds and seek to its
  // end so the stream stays aligned. min() is a no-op for valid WMOs (nDoodadSets == size/32).
  std::size_t const mods_end = f.getPos () + size;
  std::size_t const set_count = std::min<std::size_t> (nDoodadSets, size / 32);
  doodadsets.reserve(set_count);
  for (size_t i (0); i < set_count; ++i) {
    WMODoodadSet dds;
    f.read (&dds, 32);
    doodadsets.push_back (dds);
  }
  f.seek (mods_end);

  // - MODN ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MODN');

  if (size)
  {
    ddnames = reinterpret_cast<char const*> (f.getPointer ());
    f.seekRelative (size);
  }

  // - MODD ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MODD');

  // Guard a corrupt MODD chunk size. `size / 0x28` is the doodad count; a broken/custom (fuckported) WMO
  // -- e.g. Turtle's world/wmo/playerhousing/human/humanlevelonetest.wmo -- can carry a bogus MODD size of
  // over a gigabyte, which made this create TENS OF MILLIONS of wmo_doodad_instance (34.5M observed, ~7GB,
  // froze the client at load). Doodad refs (MODR) are uint16, so nothing past index 65535 is ever
  // referenceable -- clamp there. reserve() must use the clamped count too or it alone allocates GBs.
  std::size_t const modd_end = f.getPos () + size;
  std::size_t doodad_count = size / 0x28;
  constexpr std::size_t MAX_WMO_MODD_DOODADS = 65536;
  if (doodad_count > MAX_WMO_MODD_DOODADS)
  {
    LogError << "WMO \"" << _file_key.stringRepr() << "\" MODD claims " << doodad_count << " doodads "
             << "(corrupt chunk); clamping to " << MAX_WMO_MODD_DOODADS << " to avoid OOM/freeze." << std::endl;
    doodad_count = MAX_WMO_MODD_DOODADS;
  }

  modelis.reserve(doodad_count);
  for (size_t i (0); i < doodad_count; ++i)
  {
    struct
    {
      uint32_t name_offset : 24;
      uint32_t flag_AcceptProjTex : 1;
      uint32_t flag_0x2 : 1;
      uint32_t flag_0x4 : 1;
      uint32_t flag_0x8 : 1;
      uint32_t flags_unused : 4;
    } x;

    size_t after_entry (f.getPos() + 0x28);
    f.read (&x, sizeof (x));

    modelis.emplace_back(ddnames + x.name_offset, &f, _context);
    model_nearest_light_vector.emplace_back();

    f.seek (after_entry);
  }

  f.seek (modd_end); // keep chunk alignment even if the doodad count was clamped

  // - MFOG ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MFOG');

  int nfogs = size / 0x30;
  // [2026-07-24] Defensive: assert() is a NO-OP in Release, so a misaligned/garbage MFOG chunk silently made
  // nfogs balloon to ~28.7 MILLION for a Stormwind WMO -- its per-fog loop then cost ~3.4s/frame in
  // World::collect_camera_fog (0 fps in Stormwind). A real WMO has at most a few dozen fogs; reject an
  // absurd count (or a fourcc mismatch) so we neither allocate 1.4 GB nor iterate garbage every frame.
  if (fourcc != 'MFOG' || nfogs < 0 || nfogs > 4096)
  {
    LogError << "WMO: invalid MFOG chunk (fourcc mismatch or absurd nfogs=" << nfogs
             << ") -- skipping placed fog" << std::endl;
    nfogs = 0;
  }
  fogs.reserve(nfogs);

  for (size_t i (0); i < nfogs; ++i)
  {
    WMOFog fog;
    fog.init (&f);
    fogs.push_back (std::move(fog));
  }

  for (auto& group : groups)
    group.load();

  finished = true;
  _state_changed.notify_all();
}

void WMO::waitForChildrenLoaded()
{
  for (auto& tex : textures)
  {
    tex.get()->wait_until_loaded();
  }

  for (auto& doodad : modelis)
  {
    doodad.model->wait_until_loaded();
    if (doodad.model->loading_failed())
    {
      continue;
    }
    doodad.model->waitForChildrenLoaded();
  }
}

std::vector<float> WMO::intersect (math::ray const& ray, bool do_exterior) const
{
  std::vector<float> results;

  if (!finishedLoading() || loading_failed())
  {
    return results;
  }

  for (auto& group : groups)
  {
    if (!do_exterior && !group.is_indoor())
          continue;

    group.intersect (ray, &results);
  }

  if (!do_exterior && results.size())
  {
      // dirty way to find the furthest face and ignore invisible faces, cleaner way would be to do a direction check on faces
      // float max = *std::max_element(std::begin(results), std::end(results));
      // results.clear();
      // results.push_back(max);

      // other way, ignore the closest intersect, works well
      if (results.size() > 1)
      {
        auto it = std::min_element(results.begin(), results.end());
        results.erase(it);
      }
  }

  return results;
}



std::map<uint32_t, std::vector<wmo_doodad_instance>> WMO::doodads_per_group(uint16_t doodadset) const
{
  std::map<uint32_t, std::vector<wmo_doodad_instance>> doodads;

  // Doodad set 0 is the GLOBAL/default set and is ALWAYS rendered; a non-zero instance set is drawn IN
  // ADDITION to it (the client unions set 0 + the selected set). Previously only the selected set's range
  // was collected, so every interior that uses a non-zero doodad set lost all of its global props
  // (chandeliers, furniture, etc.). An out-of-range set now falls back to the global set instead of empty.
  auto const in_set = [&](uint16_t ref, uint16_t set_index) -> bool
  {
    if (set_index >= doodadsets.size())
    {
      return false;
    }
    auto const& dset = doodadsets[set_index];
    uint32_t const start = dset.start, end = start + dset.size;
    return ref >= start && ref < end;
  };

  if (doodadset >= doodadsets.size())
  {
    LogError << "Invalid doodadset " << doodadset << " for instance of wmo " << _file_key.stringRepr()
             << " -- rendering the global set 0 only" << std::endl;
  }

  // Hard cap on the number of doodad instances one WMO can spawn. A broken/custom (fuckported) WMO with a
  // corrupt MODR/MODD or an oversized group/modelis count can otherwise reference the same modelis entries
  // millions of times: observed 34.5 MILLION wmo_doodad_instances (~7GB, ~200B each) from a single custom
  // playerhousing WMO, which froze the client and blew out RAM as they were created/destroyed on the main
  // thread. Real WMOs have at most a few thousand doodads, so 200k is astronomically safe.
  constexpr std::size_t MAX_WMO_DOODADS = 200000;
  std::size_t total = 0;
  bool capped = false;

  for (int i = 0; i < groups.size() && !capped; ++i)
  {
    for (uint16_t ref : groups[i].doodad_ref())
    {
      if (ref >= modelis.size())
      {
        continue;
      }
      bool const in_global = in_set(ref, 0);
      bool const in_selected = (doodadset != 0) && in_set(ref, doodadset);
      if (in_global || in_selected)
      {
        if (total >= MAX_WMO_DOODADS)
        {
          capped = true;
          break;
        }
        doodads[i].push_back(modelis[ref]);
        ++total;
      }
    }
  }

  if (capped)
  {
    LogError << "WMO \"" << _file_key.stringRepr() << "\" resolved an absurd doodad count (>"
             << MAX_WMO_DOODADS << "); capping to avoid OOM/freeze. The WMO's MODR/MODD data is likely "
             << "corrupt (groups=" << groups.size() << ", modelis=" << modelis.size() << ")." << std::endl;
  }

  return doodads;
}

void WMOLight::init(BlizzardArchive::ClientFile* f)
{
  char type[4];
  f->read(&type, 4);
  f->read(&color, 4);
  f->read(&pos, 12);
  f->read(&intensity, 4);
  f->read(unk, 4 * 5);
  f->read(&r, 4);

  pos = glm::vec3(pos.x, pos.z, -pos.y);

  // rgb? bgr? hm
  float fa = ((color & 0xff000000) >> 24) / 255.0f;
  float fr = ((color & 0x00ff0000) >> 16) / 255.0f;
  float fg = ((color & 0x0000ff00) >> 8) / 255.0f;
  float fb = ((color & 0x000000ff)) / 255.0f;

  fcolor = glm::vec4(fr, fg, fb, fa);
  fcolor *= intensity;
  fcolor.w = 1.0f;

  /*
  // light logging
  gLog("Light %08x @ (%4.2f,%4.2f,%4.2f)\t %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f\t(%d,%d,%d,%d)\n",
  color, pos.x, pos.y, pos.z, intensity,
  unk[0], unk[1], unk[2], unk[3], unk[4], r,
  type[0], type[1], type[2], type[3]);
  */
}

void WMOLight::setup(GLint)
{
  // not used right now -_-
}

void WMOLight::setupOnce(GLint, glm::vec3, glm::vec3)
{
  //glm::vec4position(dir, 0);
  //glm::vec4position(0,1,0,0);

  //glm::vec4ambient = glm::vec4(light_color * 0.3f, 1);
  //glm::vec4diffuse = glm::vec4(light_color, 1);


  //gl.enable(light);
}



WMOGroup::WMOGroup(WMO *_wmo, BlizzardArchive::ClientFile* f, int _num, char const* names)
  : wmo(_wmo)
  , num(_num)
  , _renderer(this)
{
  // extract group info from f
  std::uint32_t flags;
  f->read(&flags, 4);
  mogi_flags = flags; // root-side MOGI flags -- AttenTransVerts tests TARGET groups' 0x48 bits
  float ff[3];
  f->read(ff, 12);
  VertexBoxMax = glm::vec3(ff[0], ff[1], ff[2]);
  f->read(ff, 12);
  VertexBoxMin = glm::vec3(ff[0], ff[1], ff[2]);
  int nameOfs;
  f->read(&nameOfs, 4);

  //! \todo  get proper name from group header and/or dbc?
  if (nameOfs > 0) {
    name = std::string(names + nameOfs);
  }
  else name = "(no name)";
}

WMOGroup::WMOGroup(WMOGroup const& other)
  : mogi_flags(other.mogi_flags)
  , BoundingBoxMin(other.BoundingBoxMin)
  , BoundingBoxMax(other.BoundingBoxMax)
  , VertexBoxMin(other.VertexBoxMin)
  , VertexBoxMax(other.VertexBoxMax)
  , use_outdoor_lights(other.use_outdoor_lights)
  , name(other.name)
  , wmo(other.wmo)
  , header(other.header)
  , center(other.center)
  , rad(other.rad)
  , num(other.num)
  , fog(other.fog)
  , _doodad_ref(other._doodad_ref)
  , _light_refs(other._light_refs)
  , _batches(other._batches)
  , _vertices(other._vertices)
  , _normals(other._normals)
  , _texcoords(other._texcoords)
  , _texcoords_2(other._texcoords_2)
  , _vertex_colors(other._vertex_colors)
  , _indices(other._indices)
  , _renderer(this)
{
  if (other.lq)
  {
    lq = std::make_unique<wmo_liquid>(*other.lq.get());
  }
}

namespace
{
  glm::vec4 colorFromInt(unsigned int col)
  {
    GLubyte r, g, b, a;
    a = (col & 0xFF000000) >> 24;
    r = (col & 0x00FF0000) >> 16;
    g = (col & 0x0000FF00) >> 8;
    b = (col & 0x000000FF);
    return glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
  }
}


void WMOGroup::load()
{
  // open group file
  std::stringstream curNum;
  curNum << "_" << std::setw (3) << std::setfill ('0') << num;

  std::string fname = wmo->file_key().filepath();
  fname.insert (fname.find (".wmo"), curNum.str ());

  BlizzardArchive::ClientFile f(fname, Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof()) {
    LogError << "Error loading WMO \"" << fname << "\"." << std::endl;
    return;
  }

  uint32_t fourcc;
  uint32_t size;

  // - MVER ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  uint32_t version;

  f.read (&version, 4);

  assert (fourcc == 'MVER' && version == 17);

  // - MOGP ----------------------------------------------

  f.read (&fourcc, 4);
  f.seekRelative (4);

  assert (fourcc == 'MOGP');

  f.read (&header, sizeof (wmo_group_header));

  unsigned fog_index = header.fogs[0];

  // downport hack
  if (fog_index >= wmo->fogs.size())
  {
      fog_index = 0;
  }
  WMOFog &wf = wmo->fogs[fog_index];

  if (wf.r2 <= 0) fog = -1; // default outdoor fog..?
  else fog = header.fogs[0];

  BoundingBoxMin = ::glm::vec3 (header.box1[0], header.box1[2], -header.box1[1]);
  BoundingBoxMax = ::glm::vec3 (header.box2[0], header.box2[2], -header.box2[1]);

  // - MOPY ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOPY');
  f.seekRelative (size);

  // - MOVI ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVI');

  _indices.resize (size / sizeof (uint16_t));

  f.read (_indices.data (), size);

  // - MOVT ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOVT');

  // let's hope it's padded to 12 bytes, not 16...
  ::glm::vec3 const* vertices = reinterpret_cast< ::glm::vec3 const*>(f.getPointer ());

  VertexBoxMin = ::glm::vec3 (std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  VertexBoxMax = ::glm::vec3 (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

  rad = 0;

  _vertices.resize(size / sizeof (::glm::vec3));

  for (size_t i = 0; i < _vertices.size(); ++i)
  {
    _vertices[i] = glm::vec3(vertices[i].x, vertices[i].z, -vertices[i].y);

    ::glm::vec3& v = _vertices[i];

    if (v.x < VertexBoxMin.x) VertexBoxMin.x = v.x;
    if (v.y < VertexBoxMin.y) VertexBoxMin.y = v.y;
    if (v.z < VertexBoxMin.z) VertexBoxMin.z = v.z;
    if (v.x > VertexBoxMax.x) VertexBoxMax.x = v.x;
    if (v.y > VertexBoxMax.y) VertexBoxMax.y = v.y;
    if (v.z > VertexBoxMax.z) VertexBoxMax.z = v.z;
  }

  center = (VertexBoxMax + VertexBoxMin) * 0.5f;
  rad = (VertexBoxMax - center).length () + 300.0f;;

  f.seekRelative (size);

  // - MONR ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MONR');

  _normals.resize (size / sizeof (::glm::vec3));

  f.read (_normals.data(), size);

  for (auto& n : _normals)
  {
    n = {n.x, n.z, -n.y};
  }

  // - MOTV ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOTV');

  _texcoords.resize (size / sizeof (glm::vec2));

  f.read (_texcoords.data (), size);

  // - MOBA ----------------------------------------------

  f.read (&fourcc, 4);
  f.read (&size, 4);

  assert (fourcc == 'MOBA');

  _batches.resize (size / sizeof (wmo_batch));
  f.read (_batches.data (), size);

  // NOTE: initRenderBatches() is deferred to the END of load() so it runs after MOCV is parsed
  // (MOCV comes after MOBA in the file).

  // - MOLR ----------------------------------------------
  if (header.flags.has_light)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOLR')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      // Per-group light references: indices into the root MOLT list naming which lights illuminate
      // THIS group (the client's per-room lighting). Used by WorldRender to scope the point-light
      // UBO per interior group instead of the global nearest-16 pool.
      _light_refs.resize (size / sizeof (int16_t));
      f.read (_light_refs.data (), _light_refs.size () * sizeof (int16_t));
    }

  }
  // - MODR ----------------------------------------------
  if (header.flags.has_doodads)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MODR')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      // Guard a corrupt MODR chunk size: a huge count would allocate gigabytes and feed the doodad
      // explosion in WMO::doodads_per_group. A group realistically has at most a few thousand refs.
      std::uint32_t count = size / sizeof (int16_t);
      constexpr std::uint32_t MAX_DOODAD_REFS = 1000000u;
      std::size_t const chunk_end = f.getPos () + size;
      if (count > MAX_DOODAD_REFS)
      {
        LogError << "WMO group MODR ref count " << count << " is absurd; clamping to " << MAX_DOODAD_REFS
                 << " (corrupt chunk)." << std::endl;
        count = MAX_DOODAD_REFS;
      }
      _doodad_ref.resize (count);
      f.read (_doodad_ref.data (), count * sizeof (int16_t));
      f.seek (chunk_end); // keep chunk alignment even if we clamped a corrupt count
    }

  }
  // - MOBN ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOBN')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative(size);
    }

  }
  // - MOBR ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOBR')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
      // std::vector<uint16_t> bsp_indices;
      // bsp_indices.resize(size / sizeof(uint16_t));
      // f.read(bsp_indices.data(), size);
      // _bsp_indices = bsp_indices;
    }
  }
  
  if (header.flags.flag_0x400)
  {
    // - MPBV ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MPBP ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBP')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MPBI ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBI')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MPBG ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MPBG')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {

      f.seekRelative (size);
    }
  }
  // - MOCV ----------------------------------------------
  if (header.flags.has_vertex_color)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOCV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      load_mocv(f, size);
    }

  }
  // - MLIQ ----------------------------------------------
  if (header.flags.has_water)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MLIQ')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      WMOLiquidHeader hlq;
      f.read(&hlq, 0x1E);

      // Interior WMO water takes its RGB from the WMO material's baked MOMT.diffColor (client
      // FUN_006b6420), NOT the zone day/night water light. A group is "outdoor" water only when an
      // EXTERIOR / exterior-lit MOGP flag is set (& 0x48); otherwise it's indoor. diffColor is a
      // D3DCOLOR stored BGRA in the file, so the raw bytes land in CArgb as r=Blue, g=Green, b=Red --
      // the true linear RGB is therefore (b, g, r). Verified against the real Timbermaw WMO: bytes
      // (46,29,25) -> RGB(25,29,46), the dark blue seen in-game (vs the green Felwood zone water).
      bool const interior_water = !(header.flags.exterior || header.flags.exterior_lit);
      bool use_material_color = false;
      glm::vec3 material_color(0.0f);
      if (interior_water
          && hlq.material_id >= 0
          && static_cast<std::size_t>(hlq.material_id) < wmo->materials.size())
      {
        auto const& dc = wmo->materials[hlq.material_id].diffuse_color;
        material_color = glm::vec3(dc.b, dc.g, dc.r) / 255.0f;
        use_material_color = true;
      }

      lq = std::make_unique<wmo_liquid> ( &f
          , hlq
          , header.group_liquid
          , (bool)wmo->flags.use_liquid_type_dbc_id
          , (bool)header.flags.ocean
          , fname
          , use_material_color
          , material_color
      );

      // creating the wmo liquid doesn't move the position
      f.seekRelative(size - 0x1E);
    }

  }
  if (header.flags.has_mori_morb)
  {
    // - MORI ----------------------------------------------
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MORI')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

    // - MORB ----------------------------------------------
    f.read(&fourcc, 4);
    f.read(&size, 4);

    if (fourcc != 'MORB')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      f.seekRelative (size);
    }

  }

  // - MOTV ----------------------------------------------
  if (header.flags.has_two_motv)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOTV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      _texcoords_2.resize(size / sizeof(glm::vec2));
      f.read(_texcoords_2.data(), size);
    }

  }
  // - MOCV ----------------------------------------------
  if (header.flags.use_mocv2_for_texture_blending)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);

    if (fourcc != 'MOCV')
    {
      LogError << "Broken header in WMO \"" << fname << "\". Trying to continue reading." << std::endl;
      f.seek (f.getPos() - 8);
    }
    else
    {
      std::vector<CImVector> mocv_2(size / sizeof(CImVector));
      f.read(mocv_2.data(), size);

      for (int i = 0; i < mocv_2.size(); ++i)
      {
        float alpha = static_cast<float>(mocv_2[i].a) / 255.f;

        // the second mocv is used for texture blending only
        if (header.flags.has_vertex_color)
        {
          _vertex_colors[i].w = alpha;
        }
        else // no vertex coloring, only texture blending with the alpha
        {
          _vertex_colors.emplace_back(0.f, 0.f, 0.f, alpha);
        }
      }
    }

  }

  //dl_light = 0;
  // "real" lighting?
  if (header.flags.indoor && header.flags.has_vertex_color)
  {
    ::glm::vec3 dirmin(1, 1, 1);
    float lenmin;

    for (auto doodad : _doodad_ref)
    {
      if (doodad >= wmo->modelis.size())
      {
          continue;
          LogError << "The WMO file currently loaded is potentially corrupt. Non-existing doodad referenced." << std::endl;
      }

      lenmin = 999999.0f * 999999.0f;
      ModelInstance& mi = wmo->modelis[doodad];
      for (unsigned int j = 0; j < wmo->lights.size(); j++)
      {
        WMOLight& l = wmo->lights[j];
        ::glm::vec3 dir = l.pos - mi.pos;

        float ll = glm::length(dir) * glm::length(dir);
        if (ll < lenmin)
        {
          lenmin = ll;
          dirmin = dir;
        }
      }
      wmo->model_nearest_light_vector[doodad] = dirmin;
    }

    use_outdoor_lights = false;
  }
  else
  {
    use_outdoor_lights = true;
  }

  // Retain a compact copy of the baked MOCV rgb for TRUE indoor groups (client proxy rule:
  // flags & (exterior|exterior_lit) == 0): interior ground-colour sampling for units
  // (sample_ground_color / RE_notes/15) needs it on the CPU after the renderer clears
  // _vertex_colors on upload.
  if (header.flags.indoor && !header.flags.exterior && !header.flags.exterior_lit
      && header.flags.has_vertex_color && _vertex_colors.size() >= _vertices.size())
  {
    // CANON (LIGHT_FOG_SELECTION_RE.md §9.3): the client's interior MOCV CARRIES the MOHD ambient, so an
    // M2/gameobject sampling a dark floor is floored to the room ambient and NEVER samples pure (0,0,0)
    // (FUN_006a77e0 can't lift a 0 -> that would render solid black). noggit's fix_vertex_color_alpha
    // SUBTRACTS the ambient (the WMO face shader adds it back); the CPU ground sample must add it back too,
    // or a MOCV<=ambient vertex reads (0,0,0) and the gameobject renders black (Ironforge dark-floor props;
    // IF MOHD ambient = (5,5,14)/255). So store _ground_colors = the floor's LIT colour = eff_ambient +
    // fixed MOCV, exactly the interior FACE branch. (Near-white MOHD ambient = the "MOCV already carries
    // full light" sentinel -> 0.04 floor, matching the shader's interior_ambient guard.)
    glm::vec3 const mohd_amb = glm::vec3(wmo->ambient_light_color);
    bool const neutral_amb = mohd_amb.x > 0.95f && mohd_amb.y > 0.95f && mohd_amb.z > 0.95f;
    glm::vec3 const eff_amb = neutral_amb ? glm::vec3(0.04f) : mohd_amb;
    _ground_colors.resize(_vertices.size());
    // GAP B (checklist 8.7): store the PRISTINE baked MOCV floor alpha per vertex [0..255] alongside the
    // rgb, so sample_ground_color() can drive the doorway day/night spill. Sourced from the pristine
    // capture (colorFromInt), NOT _vertex_colors[i].w (already clobbered by fix/portal above). alpha 0
    // for verts with no MOCV.
    _ground_alphas.resize(_vertices.size());
    for (std::size_t i = 0; i < _vertices.size(); ++i)
    {
      _ground_colors[i] = glm::u8vec3(static_cast<std::uint8_t>(glm::clamp(eff_amb.x + _vertex_colors[i].x, 0.f, 1.f) * 255.f)
                                    , static_cast<std::uint8_t>(glm::clamp(eff_amb.y + _vertex_colors[i].y, 0.f, 1.f) * 255.f)
                                    , static_cast<std::uint8_t>(glm::clamp(eff_amb.z + _vertex_colors[i].z, 0.f, 1.f) * 255.f));
      float const pa = (i < _mocv_pristine_alpha.size()) ? _mocv_pristine_alpha[i] : 0.f;
      _ground_alphas[i] = static_cast<std::uint8_t>(glm::clamp(pa, 0.f, 1.f) * 255.f + 0.5f);
    }
  }

  // Deferred from just after the MOBA read: build render batches now that MOCV has been parsed.
  _renderer.initRenderBatches();
}

bool WMOGroup::sample_ground_color(glm::vec3 const& local_pos, glm::vec3* out, float* out_alpha) const
{
  // Client CWorldEntity::SampleGroundColor (wow.exe 0x69E4C0 -> 0x6B9A50, RE_notes/15): ray from
  // entity+1.0 down 12.0 units, barycentric-interpolate the hit face's MOCV. Group verts are stored in
  // noggit convention (y = up), so "down" is -y here; a WMO instance's rotation is assumed yaw-only
  // (true for buildings), which preserves the vertical.
  if (_ground_colors.empty() || _vertices.empty() || _indices.size() < 3)
  {
    return false;
  }

  float const top = local_pos.y + 1.0f;
  float const bottom = local_pos.y - 12.0f;
  float best_y = bottom;
  bool found = false;

  for (std::size_t i = 0; i + 2 < _indices.size(); i += 3)
  {
    std::uint16_t const ia = _indices[i], ib = _indices[i + 1], ic = _indices[i + 2];
    if (ia >= _vertices.size() || ib >= _vertices.size() || ic >= _vertices.size())
    {
      continue;
    }
    glm::vec3 const& a = _vertices[ia];
    glm::vec3 const& b = _vertices[ib];
    glm::vec3 const& c = _vertices[ic];

    // Horizontal (xz) barycentric test -- the client projects the hit onto the face's dominant plane,
    // which for a floor face is the horizontal one.
    float const den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::abs(den) < 1e-6f)
    {
      continue; // vertical face (wall): no horizontal footprint to stand on
    }
    float const l0 = ((b.z - c.z) * (local_pos.x - c.x) + (c.x - b.x) * (local_pos.z - c.z)) / den;
    float const l1 = ((c.z - a.z) * (local_pos.x - c.x) + (a.x - c.x) * (local_pos.z - c.z)) / den;
    float const l2 = 1.f - l0 - l1;
    if (l0 < -0.001f || l1 < -0.001f || l2 < -0.001f)
    {
      continue;
    }

    float const y = l0 * a.y + l1 * b.y + l2 * c.y;
    if (y > top || y <= best_y)
    {
      continue; // above the entity's feet, below the 12-unit reach, or below a closer floor already found
    }

    // The client clamps the fixed-point barycentrics to [0,256]; mirror with a [0,1] clamp.
    float const w0 = glm::clamp(l0, 0.f, 1.f), w1 = glm::clamp(l1, 0.f, 1.f), w2 = glm::clamp(l2, 0.f, 1.f);
    glm::vec3 const ca = glm::vec3(_ground_colors[ia]) / 255.f;
    glm::vec3 const cb = glm::vec3(_ground_colors[ib]) / 255.f;
    glm::vec3 const cc = glm::vec3(_ground_colors[ic]) / 255.f;
    *out = ca * w0 + cb * w1 + cc * w2;
    // GAP B (checklist 8.7): same barycentric weights interpolate the pristine baked floor alpha [0..1]
    // for the doorway day/night spill. Bounds-guarded like the rgb path (ia/ib/ic already < _vertices).
    if (out_alpha && !_ground_alphas.empty()
        && ia < _ground_alphas.size() && ib < _ground_alphas.size() && ic < _ground_alphas.size())
    {
      *out_alpha = (static_cast<float>(_ground_alphas[ia]) * w0
                  + static_cast<float>(_ground_alphas[ib]) * w1
                  + static_cast<float>(_ground_alphas[ic]) * w2) / 255.f;
    }
    best_y = y;
    found = true;
  }

  return found;
}

void WMOGroup::load_mocv(BlizzardArchive::ClientFile& f, uint32_t size)
{
  std::uint32_t const count = size / sizeof(std::uint32_t);
  std::vector<std::uint32_t> colors(count);
  std::memcpy(colors.data(), f.getPointer(), count * sizeof(std::uint32_t));

  // REVERTED (user, 2026-07-07): the byte-matched raw-MOCV pipeline (atten_trans_verts +
  // tex*MOCV*(1+4a), RE_notes/19) rendered WORSE on our content despite matching the client's vertex
  // bytes -- the rest of our pipeline evidently compensates around the legacy fix. Back to the
  // approved formula; atten_trans_verts stays parked below for a future retry.
  _vertex_colors.resize(count);
  // GAP B (checklist 8.7): capture the PRISTINE baked MOCV alpha now, before fix_vertex_color_alpha
  // (sets .w=1) or compute_portal_openness (sets .w=portal-fade) overwrite _vertex_colors[i].w below.
  // The _ground_alphas build at the end of load() sources the doorway-spill exposure from this.
  _mocv_pristine_alpha.resize(count);
  for (std::size_t i(0); i < count; ++i)
  {
    _vertex_colors[i] = colorFromInt(colors[i]);
    _mocv_pristine_alpha[i] = _vertex_colors[i].w;
  }

  // 3.3.5a WMO interior: the client runs CMapObjGroup::FixColorVertexAlpha (a WotLK-era MOCV transform 1.12
  // lacks) and its interior material shader is mod2x (tex*MOCV*2). noggit is tuned for 1.12 (verbatim MOCV,
  // x1). Gate the WotLK path to non-CLASSIC projects behind an env toggle (interior lighting = A/B, never a
  // blind change; RE notes warn the >1 combine can clamp-blow on our pipeline). Evaluated once.
  // DEFAULT-ON for non-CLASSIC (user-confirmed brighter, 2026-07-29); opt out with NOGGIT_NO_335A_WMO_MOD2X=1.
  static bool const s_wotlk_wmo_mod2x =
      (std::getenv("NOGGIT_NO_335A_WMO_MOD2X") == nullptr)
   && Noggit::Project::CurrentProject::get() != nullptr
   && Noggit::Project::CurrentProject::get()->projectVersion != Noggit::Project::ProjectVersion::CLASSIC;

  if (wmo->flags.do_not_fix_vertex_color_alpha)
  {
    // MOHD flag 0x08 -> the client SKIPS FixColorVertexAlpha (both versions); MOCV stays verbatim. Under
    // mod2x that means these faces render tex*MOCV*2 (2x) -- the shader x2 rides on top.
    int interior_batchs_start = 0;

    if (header.transparency_batches_count > 0)
    {
      interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
    }

    for (int n = interior_batchs_start; n < _vertex_colors.size(); ++n)
    {
      _vertex_colors[n].w = header.flags.exterior ? 1.f : 0.f;
    }
  }
  else if (s_wotlk_wmo_mod2x)
  {
    fix_vertex_color_alpha_wotlk();
  }
  else
  {
    fix_vertex_color_alpha();
  }

  compute_portal_openness();

  // there's no read so this is required
  f.seekRelative(size);
}

void WMOGroup::fix_vertex_color_alpha()
{
  // Interior = x1 * MOCV (working approximation). The byte-exact tex*MOCV*(1+4a) HDR path blows out on our
  // content (noggit renders tex*MOCV brighter than the client), so we keep the x1 approximation and, for the
  // near-white (~1,1,1) MOHD sentinel WMOs (Timbermaw), fold the (1+4a) overbright in at LOAD (clamped) so the
  // skywrap glows. Normal WMOs stay verbatim; alpha forced to 1 (the shader uses x1*MOCV and the portal spill
  // rides the alpha channel separately via compute_portal_openness).
  glm::vec4 const amb = wmo->flags.use_unified_render_path ? glm::vec4(0.f) : wmo->ambient_light_color;
  bool const neutral_ambient = amb.x > 0.95f && amb.y > 0.95f && amb.z > 0.95f;

  int interior_batchs_start = 0;
  if (header.transparency_batches_count > 0)
  {
    interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
  }
  constexpr float normalized_alpha_scale = 255.f / 64.f;

  for (std::size_t i = 0; i < _vertex_colors.size(); ++i)
  {
    auto& color = _vertex_colors[i];
    float r = color.x;
    float g = color.y;
    float b = color.z;
    float const a = color.w;

    if (neutral_ambient)
    {
      if (static_cast<int>(i) >= interior_batchs_start)
      {
        r = r + r * a * normalized_alpha_scale;   // (1+4a) overbright -> the Timbermaw skywrap glow
        g = g + g * a * normalized_alpha_scale;
        b = b + b * a * normalized_alpha_scale;
      }
      else
      {
        r = r * (1.f - a);
        g = g * (1.f - a);
        b = b * (1.f - a);
      }
    }
    // else: NORMAL WMO -> leave RGB verbatim.

    color.x = std::min(1.f, std::max(0.f, r));
    color.y = std::min(1.f, std::max(0.f, g));
    color.z = std::min(1.f, std::max(0.f, b));
    color.w = 1.f;
  }
}

void WMOGroup::fix_vertex_color_alpha_wotlk()
{
  // Byte-exact port of CMapObjGroup::FixColorVertexAlpha from the stock 3.3.5a client (12340, FUN_007D7380).
  // Operates on the 0..255 CImVector MOCV. begin_second_fixup splits transparency-batch verts from interior
  // verts; the two ranges get DIFFERENT arithmetic. Colours here are normalized floats -> quantize to 0..255,
  // apply the integer ops, renormalize. `color.w` still holds the PRISTINE MOCV alpha at this point (set by
  // load_mocv before this runs). Pairs with the mod2x (tex*MOCV*2) interior combine in wmo_frag.
  int begin_second_fixup = 0;
  if (header.transparency_batches_count > 0)
  {
    begin_second_fixup = _batches[header.transparency_batches_count - 1].vertex_end + 1;
  }

  auto const q = [](float v) -> int { return std::min(255, std::max(0, static_cast<int>(std::lround(v * 255.f)))); };

  for (std::size_t i = 0; i < _vertex_colors.size(); ++i)
  {
    auto& color = _vertex_colors[i];
    int r = q(color.x);
    int g = q(color.y);
    int b = q(color.z);
    int const a = q(color.w);

    if (static_cast<int>(i) < begin_second_fixup)
    {
      // transparency-batch verts: rgb >>= 1 (halve); alpha unchanged. (Net under mod2x = x1.)
      r >>= 1;
      g >>= 1;
      b >>= 1;
    }
    else
    {
      // interior verts: c = min(255, ((c*a >> 6) + c) >> 1); alpha forced to 255.
      // (Net under mod2x: a=0 -> x1; higher alpha -> brighter, up to the framebuffer ceiling.)
      r = std::min(255, (((r * a) >> 6) + r) >> 1);
      g = std::min(255, (((g * a) >> 6) + g) >> 1);
      b = std::min(255, (((b * a) >> 6) + b) >> 1);
    }

    color.x = static_cast<float>(r) / 255.f;
    color.y = static_cast<float>(g) / 255.f;
    color.z = static_cast<float>(b) / 255.f;
    color.w = 1.f; // interior forced to 255; trans verts don't use alpha downstream (portal openness overwrites)
  }
}

namespace
{
  // Forward decls -- defined in the anonymous namespace below (shared with atten_trans_verts); the canon
  // openness computation in compute_portal_openness needs them and sits above their definitions.
  int major_axis(glm::vec3 const& n);
  bool point_in_poly_2d(glm::vec3 const& v, glm::vec3 const* poly, std::size_t n, int drop_axis);
  float dist_to_polygon_edges_3d(glm::vec3 const& v, glm::vec3 const* poly, std::size_t n);
}

void WMOGroup::compute_portal_openness()
{
  // Portal-proximity "openness" baked into the vertex-colour alpha (1 at a portal fading to 0 inward):
  // the shader lerps the interior light toward the outdoor light by it, smoothing doorways.
  // The `do_not_attenuate_vertices_based_on_distance_to_portal` (MOHD flag 0x1) guard was REMOVED
  // (2026-07-27, DIFFERENTIAL TEST vs the real client): Stormwind.wmo sets flag 0x1, yet a side-by-side
  // screenshot of the actual Turtle client shows it DOES spill outdoor light onto the Cathedral doorway
  // reveal -- warm/bright at the opening, fading to the cool interior. So flag 0x1 does NOT disable the WMO
  // doorway light-spill; gating on it left Stormwind's (and every 0x1 WMO's) doorways flat and cool, which
  // is the "doesn't blend with the outside light" the user saw. The portal-distance falloff and the
  // exterior-target-portal test below already scope the spill to real openings, so honouring the flag here
  // was pure regression.
  if (!header.flags.indoor
      || header.flags.use_mocv2_for_texture_blending
      || header.portal_count == 0
      || _vertices.empty()
      || _vertex_colors.size() < _vertices.size())
  {
    return;
  }

  // CANON openness = the client's CMapObjGroup::AttenTransVerts `op` (verified byte-for-byte against the
  // decompiled function; the port is atten_trans_verts() below): accumulate `1 - 0.15*d` over this group's
  // MOPR portals whose TARGET group is EXTERIOR (mogi_flags & 0x48), d = 3D distance to the portal polygon;
  // a vertex ON a portal into another INTERIOR group zeroes it. clamp [0,1]. Stored per-vertex in the MOCV
  // alpha; the shader brightens the interior light toward WHITE by it (AttenTransVerts does rgb+=(255-rgb)*op),
  // so a doorway threshold reaches full texture brightness and fades inward exactly where the client's does.
  //
  // SCOPE = EVERY vertex of this (indoor, portal-bearing) group -- NOT just the doorway-transition band. The
  // `1 - 0.15*d` distance falloff is precisely what makes light coming through the opening CONTINUE onto the
  // interior floor and walls and FADE inward over ~6.7 units, so the room reads as ONE connected space lit
  // through the doorway -- instead of the reveal being a separate lit patch that hard-cuts at the mesh seam
  // (the user's real complaint: the outdoor light "shouldn't just cut off" at the boundary; the pieces must
  // light together, not individually and get stitched). Restricting op to transparency-batch verts confined
  // the spill to the short reveal and destroyed exactly that continuity. Deep interior verts (d > 6.7)
  // accumulate 0 -> no spill, so this does NOT flood whole rooms; non-portal groups early-return above, and
  // their op=1.0-default blowout is separately gated by eWMOBatch_PortalSpill in the shader. Stored in MOCV a.
  for (std::size_t i = 0; i < _vertices.size(); ++i)
  {
    glm::vec3 const& v = _vertices[i];
    float accum = 0.0f;
    for (std::size_t r = header.portal_start;
         r < static_cast<std::size_t>(header.portal_start) + header.portal_count
         && r < wmo->_portal_refs.size(); ++r)
    {
      auto const& ref = wmo->_portal_refs[r];
      if (ref.portal < 0 || static_cast<std::size_t>(ref.portal) >= wmo->_portal_info.size()) { continue; }
      auto const& portal = wmo->_portal_info[static_cast<std::size_t>(ref.portal)];
      if (portal.vertex_count == 0
          || static_cast<std::size_t>(portal.base_vertex) + portal.vertex_count > wmo->_portal_vertices.size())
      {
        continue;
      }
      glm::vec3 const* poly = wmo->_portal_vertices.data() + portal.base_vertex;
      float const d = glm::dot(portal.plane_normal, v) + portal.plane_dist;
      float d_use;
      if (std::abs(d) <= 0.01f
          && point_in_poly_2d(v, poly, portal.vertex_count, major_axis(portal.plane_normal)))
      {
        d_use = static_cast<float>(ref.dir) * d;
      }
      else
      {
        d_use = dist_to_polygon_edges_3d(v, poly, portal.vertex_count);
      }
      bool const target_exterior = ref.group >= 0
        && static_cast<std::size_t>(ref.group) < wmo->groups.size()
        && (wmo->groups[static_cast<std::size_t>(ref.group)].mogi_flags & 0x48);
      if (target_exterior)
      {
        float const v25 = (d_use >= 0.0f) ? d_use * 0.15f : 0.0f;
        if (1.0f - v25 > 0.001f) { accum += 1.0f - v25; }
      }
      else if (d_use > -1.0f && d_use < 1.0f)
      {
        accum = 0.0f;
        break;
      }
    }
    _vertex_colors[i].w = (accum > 0.001f) ? std::min(accum, 1.0f) : 0.0f;
  }
  _has_portal_openness = true;
}

namespace
{
  int major_axis(glm::vec3 const& n)
  {
    float const ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
    return (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
  }

  // Even-odd crossing test on the two coordinates left after dropping the plane's major axis.
  bool point_in_poly_2d(glm::vec3 const& v, glm::vec3 const* poly, std::size_t n, int drop_axis)
  {
    int const a0 = (drop_axis == 0) ? 1 : 0;
    int const a1 = (drop_axis == 2) ? 1 : 2;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++)
    {
      float const xi = poly[i][a0], yi = poly[i][a1];
      float const xj = poly[j][a0], yj = poly[j][a1];
      if (((yi > v[a1]) != (yj > v[a1]))
          && (v[a0] < (xj - xi) * (v[a1] - yi) / (yj - yi) + xi))
      {
        inside = !inside;
      }
    }
    return inside;
  }

  // Min 3D point-to-SEGMENT distance over the polygon's edges (from the UNPROJECTED vertex).
  float dist_to_polygon_edges_3d(glm::vec3 const& v, glm::vec3 const* poly, std::size_t n)
  {
    float best = std::numeric_limits<float>::max();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++)
    {
      glm::vec3 const& a = poly[j];
      glm::vec3 const& b = poly[i];
      glm::vec3 const ab = b - a;
      float const len2 = glm::dot(ab, ab);
      float const t = len2 > 0.f ? std::clamp(glm::dot(v - a, ab) / len2, 0.f, 1.f) : 0.f;
      best = std::min(best, glm::length(v - (a + ab * t)));
    }
    return best;
  }
}

void WMOGroup::atten_trans_verts(std::vector<std::uint32_t>& colors)
{
  // 1.12 CMapObj::AttenTransVerts, ported verbatim from the byte-matched spec (RE_notes/19 section 3).
  // Touches ONLY transparency-batch vertices: brighten toward white by proximity to portals whose
  // TARGET group is exterior (MOGI flags & 0x48), op = 1 - 0.15*d accumulated over portals, capped at
  // 1; a vertex ON a portal to another INTERIOR group is zeroed. Written ONLY when the new alpha byte
  // strictly exceeds the stored one (authored MOCV usually already bakes this -- left bit-identical).
  if (header.transparency_batches_count == 0
      || header.transparency_batches_count > _batches.size())
  {
    return;
  }
  std::size_t const n_trans = std::min<std::size_t>(
      static_cast<std::size_t>(_batches[header.transparency_batches_count - 1].vertex_end) + 1,
      std::min(colors.size(), _vertices.size()));

  for (std::size_t vi = 0; vi < n_trans; ++vi)
  {
    glm::vec3 const& v = _vertices[vi];
    float accum = 0.0f;

    for (std::size_t r = header.portal_start;
         r < static_cast<std::size_t>(header.portal_start) + header.portal_count
         && r < wmo->_portal_refs.size(); ++r)                       // MOPR order matters
    {
      auto const& ref = wmo->_portal_refs[r];
      if (ref.portal < 0 || static_cast<std::size_t>(ref.portal) >= wmo->_portal_info.size())
      {
        continue;
      }
      auto const& portal = wmo->_portal_info[static_cast<std::size_t>(ref.portal)];
      if (portal.vertex_count == 0
          || static_cast<std::size_t>(portal.base_vertex) + portal.vertex_count > wmo->_portal_vertices.size())
      {
        continue;
      }
      glm::vec3 const* poly = wmo->_portal_vertices.data() + portal.base_vertex;

      float const d = glm::dot(portal.plane_normal, v) + portal.plane_dist;

      float d_use;
      // On-plane epsilon: bracketed to (0, 0.39) by the byte-match; 0.01 recommended (note sec 7).
      if (std::abs(d) <= 0.01f
          && point_in_poly_2d(v, poly, portal.vertex_count, major_axis(portal.plane_normal)))
      {
        d_use = static_cast<float>(ref.dir) * d;   // vertex lies ON the portal: ~0
      }
      else
      {
        d_use = dist_to_polygon_edges_3d(v, poly, portal.vertex_count);
      }

      bool const target_exterior = ref.group >= 0
        && static_cast<std::size_t>(ref.group) < wmo->groups.size()
        && (wmo->groups[static_cast<std::size_t>(ref.group)].mogi_flags & 0x48);

      if (target_exterior)
      {
        float const v25 = (d_use >= 0.0f) ? d_use * 0.15f : 0.0f;   // 0.15 exactly (constraint-pinned)
        if (1.0f - v25 > 0.001f)
        {
          accum += 1.0f - v25;                     // multiple exterior portals ACCUMULATE
        }
      }
      else if (d_use > -1.0f && d_use < 1.0f)
      {
        accum = 0.0f;                              // ON a portal to an interior group: kill, stop
        break;
      }
    }

    float const op = (accum > 0.001f) ? std::min(accum, 1.0f) : 0.0f;
    std::uint8_t const na = static_cast<std::uint8_t>(op * 255.0f);  // truncation, not rounding

    std::uint32_t const c = colors[vi];
    std::uint8_t const ca = static_cast<std::uint8_t>((c >> 24) & 0xFF);
    if (na > ca)  // BYTE compare vs stored alpha; skip the whole vertex unless strictly higher
    {
      std::uint8_t const cr = static_cast<std::uint8_t>((c >> 16) & 0xFF);
      std::uint8_t const cg = static_cast<std::uint8_t>((c >> 8) & 0xFF);
      std::uint8_t const cb = static_cast<std::uint8_t>(c & 0xFF);
      std::uint8_t const nr = static_cast<std::uint8_t>(cr + (255.0f - cr) * op);
      std::uint8_t const ng = static_cast<std::uint8_t>(cg + (255.0f - cg) * op);
      std::uint8_t const nb = static_cast<std::uint8_t>(cb + (255.0f - cb) * op);
      colors[vi] = (static_cast<std::uint32_t>(na) << 24)
                 | (static_cast<std::uint32_t>(nr) << 16)
                 | (static_cast<std::uint32_t>(ng) << 8)
                 |  static_cast<std::uint32_t>(nb);
    }
  }
}


bool WMOGroup::is_visible( glm::mat4x4 const& transform
                         , math::frustum const& frustum
                         , float const& cull_distance
                         , glm::vec3 const& camera
                         , display_mode display
                         ) const
{
  std::array<glm::vec3, 8> world_corners =
  {
    transform * glm::vec4(BoundingBoxMin.x, BoundingBoxMin.y, BoundingBoxMin.z, 1.0f),
    transform * glm::vec4(BoundingBoxMin.x, BoundingBoxMin.y, BoundingBoxMax.z, 1.0f),
    transform * glm::vec4(BoundingBoxMin.x, BoundingBoxMax.y, BoundingBoxMin.z, 1.0f),
    transform * glm::vec4(BoundingBoxMin.x, BoundingBoxMax.y, BoundingBoxMax.z, 1.0f),
    transform * glm::vec4(BoundingBoxMax.x, BoundingBoxMin.y, BoundingBoxMin.z, 1.0f),
    transform * glm::vec4(BoundingBoxMax.x, BoundingBoxMin.y, BoundingBoxMax.z, 1.0f),
    transform * glm::vec4(BoundingBoxMax.x, BoundingBoxMax.y, BoundingBoxMin.z, 1.0f),
    transform * glm::vec4(BoundingBoxMax.x, BoundingBoxMax.y, BoundingBoxMax.z, 1.0f)
  };

  if (!frustum.intersects(world_corners))
  {
    return false;
  }

  glm::vec3 pos = transform * glm::vec4(center, 1);

  float dist = display == display_mode::in_3D
    ? glm::distance(pos, camera) - rad
    : std::abs(pos.y - camera.y) - rad;

  return (dist < cull_distance);
}


void WMOGroup::intersect (math::ray const& ray, std::vector<float>* results) const
{
  if (!ray.intersect_bounds (VertexBoxMin, VertexBoxMax))
  {
    return;
  }

  //! \todo Also allow clicking on doodads and liquids.
  for (auto&& batch : _batches)
  {
    for (size_t i (batch.index_start); i < batch.index_start + batch.index_count; i += 3)
    {
      // TODO : only intersect visible triangles
      // TODO : option to only check collision
      if ( auto&& distance
         = ray.intersect_triangle ( _vertices[_indices[i + 0]]
                                  , _vertices[_indices[i + 1]]
                                  , _vertices[_indices[i + 2]]
                                  )
         )
      {
        results->emplace_back (*distance);
      }
    }
  }
}

void WMOGroup::drawLiquid ( glm::mat4x4 const& transform
                          , OpenGL::Scoped::use_program& water_shader
                          , Noggit::Rendering::LiquidTextureManager& texture_manager
                          , bool // draw_fog
                          , int animtime
                          )
{
  // draw liquid
  //! \todo  culling for liquid boundingbox or something
  if (lq) 
  { 
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_TRUE> const depth_test;
    gl.enable(GL_BLEND);
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.depthMask(GL_TRUE);

    lq->draw(transform, water_shader, texture_manager, animtime);

    gl.disable(GL_BLEND);
  }
}

void WMOGroup::setupFog (bool draw_fog, std::function<void (bool)> setup_fog)
{
  if (use_outdoor_lights || fog == -1) {
    setup_fog (draw_fog);
  }
  else {
    wmo->fogs[fog].setup();
  }
}

bool WMO::evaluate_camera_fog(WMOGroup const& group, glm::mat4x4 const& transform,
                              glm::vec3 const& camera, bool camera_inside_wmo, glm::vec3* color,
                              float* fog_end, float* fog_start_abs) const
{
  // CAMERA-GROUP GATE (client FUN_0069de20 param_3 + the FUN_006be250 floor walk): the WMO camera fog
  // applies ONLY while the camera stands over an INTERIOR group's floor. camera_inside_wmo is now the
  // geometric floor test (World::camera_is_inside_wmo -- a ray-down onto indoor WMO geometry), NOT a loose
  // AABB, so an open-air tower (Karazhan's Malchezaar) resolves FALSE -> zone fog (gray), and a real
  // enclosed interior (Ironforge's Great Forge) resolves TRUE -> the WMO's own fog. This one geometric
  // rule ends the Ironforge<->Malchezaar flip-flop -- no fog[0] guessing; the floor decides.
  if (!camera_inside_wmo)
  {
    return false;
  }

  if (fogs.size() <= 1)
  {
    return false; // default-only WMO: the client evaluator bails (nFogs == 1) -> zone fog
  }

  // accumulator starts as the DEFAULT entry fogs[0] (client copies it verbatim before blending)
  auto const& f0 = fogs[0];
  glm::vec3 c = glm::vec3(f0.color);
  float end = f0.fogend;
  float start = f0.fogstart;

  struct Candidate { float dist; std::uint8_t id; };
  Candidate cand[4];
  int n = 0;
  for (int fi = 0; fi < 4; ++fi)
  {
    std::uint8_t const id = group.fog_id(fi);
    if (id == 0 || id >= fogs.size())
    {
      continue; // slot 0 references the default entry -- never a blend candidate
    }
    auto const& wf = fogs[id];
    if (wf.flags & 1)
    {
      continue; // client skips flag-1 fogs in the candidate loop
    }
    glm::vec3 const fog_world = glm::vec3(transform * glm::vec4(wf.pos, 1.0f));
    float const d = glm::distance(camera, fog_world);
    if (d >= wf.r2)
    {
      continue;
    }
    cand[n++] = {d, id};
  }

  // NO candidate in range: the per-draw traces (kara/goldshire) show the ZONE fog as the dominant
  // WMO-geometry state, with MFOG states appearing ONLY around the authored anchors -- the fogs[0]
  // default base manifests only THROUGH the blend when a placed fog is in range. Applying it
  // unconditionally painted every multi-fog WMO (and, via the camera fog, all doodads near one)
  // with heavy fog the live client never shows outside the anchors.
  if (n == 0)
  {
    // Camera is over an interior floor (gated true above) but no placed fog sphere is in range -> the WMO's
    // DEFAULT fog[0] as the base (RE @0069de20). Ironforge's Great Forge needs this long interior fog (end
    // 805.6), not the short outdoor zone fog. Malchezaar can't reach here: its open tower resolves
    // camera_inside_wmo == false and already returned the zone fog at the top.
    *color = c;
    *fog_end = end;
    *fog_start_abs = start;
    return true;
  }

  // farthest -> nearest (the client pops a max-heap), so the NEAREST fog dominates
  std::sort(cand, cand + n,
            [](Candidate const& a, Candidate const& b) { return a.dist > b.dist; });
  for (int i = 0; i < n; ++i)
  {
    auto const& wf = fogs[cand[i].id];
    float const d = std::clamp(cand[i].dist, 0.0f, wf.r2);
    // client weight @0069e1c0: full inside r1, linear falloff to 0 at r2
    float const w = d <= wf.r1 ? 1.0f : 1.0f - (d - wf.r1) / (wf.r2 - wf.r1);
    c = glm::mix(c, glm::vec3(wf.color), w);
    end = glm::mix(end, wf.fogend, w);
    start = glm::mix(start, wf.fogstart, w);
  }

  *color = c;
  *fog_end = end;
  *fog_start_abs = start;
  return true;
}

void WMOFog::init(BlizzardArchive::ClientFile* f)
{
  f->read(this, 0x30);
  color = glm::vec4(((color1 & 0x00FF0000) >> 16) / 255.0f, ((color1 & 0x0000FF00) >> 8) / 255.0f,
    (color1 & 0x000000FF) / 255.0f, ((color1 & 0xFF000000) >> 24) / 255.0f);
  float temp;
  temp = pos.y;
  pos.y = pos.z;
  pos.z = -temp;
  // RAW authored distances -- trace-verified in BOTH captures (Kara: client fog 277.8/555.6 == MFOG
  // entry end=555.6 startMult=0.5 exactly; Goldshire inn: 49.7/199 ~= entry 194.4x0.25). The legacy
  // x1.5 scaling pushed fog 50% past the authored range and made room fog invisible.
  fogstart = fogstart * fogend;
}

void WMOFog::setup()
{

}

decltype (WMOManager::_) WMOManager::_;

void WMOManager::report()
{
  std::string output = "Still in the WMO manager:\n";
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const& key, WMO const&)
            {
              output += " - " + key.stringRepr() + "\n";
            }
          );
  LogDebug << output;
}

void WMOManager::clear_hidden_wmos()
{
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const&, WMO& wmo)
            {
              wmo.show();
            }
          );
}

void WMOManager::unload_all(Noggit::NoggitRenderContext context)
{
    _.context_aware_apply(
        [&] (BlizzardArchive::Listfile::FileKey const&, WMO& wmo)
        {
            wmo.renderer()->unload();
        }
        , context
    );
}
