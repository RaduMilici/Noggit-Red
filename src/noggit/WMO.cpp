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
  // 2026-09-15: chunk DISPATCH instead of a fixed chunk order. Modern (CASC) roots drop MOTX and MODN
  // (MOMT texture fields and a MODI array carry fileDataIDs), add GFID (group files by id) and MOSI
  // (skybox by id), and interleave new chunks (MOUV, MOSI, MDAL, MAVG ...). Group files are unchanged.
  // Measured on 1.15.9 / 2.5.6 human_farm/farm.wmo -- docs/client_re/41 section 6.
  auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
  BlizzardArchive::ClientFile f(_file_key, client_data);
  if (f.isEof()) {
    LogError << "Error loading WMO \"" << _file_key.stringRepr() << "\"." << std::endl;
    return;
  }

  uint32_t fourcc;
  uint32_t size;

  float ff[3];

  char const* ddnames = nullptr;
  char const* groupnames = nullptr;
  std::vector<char> texbuf;                  // MOTX: texture names (WotLK); empty on modern roots
  bool have_motx = false;
  std::vector<std::uint32_t> doodad_ids;     // MODI: doodad model fileDataIDs (modern)
  std::size_t modd_pos = 0;                  // MODD entries are resolved after the loop (MODI follows MODD)
  std::size_t doodad_count = 0;
  std::uint32_t skybox_file_id = 0;          // MOSI

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

  // MOHD ambColor is a D3DCOLOR stored **BGRA** in the file, so the bytes land in CArgb (declared
  // r,g,b,a) as r=Blue, b=Red -- the true linear RGB is (b, g, r). Same convention already used for
  // the WMO liquid MOMT diffColor below. PROVEN by the Ascension apitrace (2026-08-12): the client
  // lights that interior with ambient (0.1843,0.1216,0.0863) = (47,31,22) WARM BROWN, while these
  // very bytes were being read as (22,31,47) COLD BLUE -- the interiors were tinted the exact
  // inverse of the authored colour. Stormwind's (33,33,33) is grey so it never exposed this.
  ambient_light_color.x = static_cast<float>(ambient_color.b) / 255.f;
  ambient_light_color.y = static_cast<float>(ambient_color.g) / 255.f;
  ambient_light_color.z = static_cast<float>(ambient_color.r) / 255.f;
  ambient_light_color.w = static_cast<float>(ambient_color.a) / 255.f;

  // note: used to map to size_t, but our other values don't support that.
  std::map<std::uint32_t, std::uint32_t> texture_offset_to_inmem_index;

  // MOTX offset (WotLK) or fileDataID (modern) -> index into `textures`
  auto load_texture
    ( [&] (std::uint32_t ofs_or_id, bool is_second_texture)
      {
        bool const by_id = !have_motx;
        bool is_empty;
        std::string texture;

        if (by_id)
        {
          is_empty = ofs_or_id == 0;
          if (is_empty)
          {
            texture = is_second_texture ? "tileset/generic/black.blp" : "textures/shanecube.blp";
          }
          else
          {
            std::string const path = client_data->listfile()->getPath(ofs_or_id);
            texture = path.empty() ? "fdid:" + std::to_string(ofs_or_id)
                                   : BlizzardArchive::ClientData::normalizeFilenameInternal(path);
          }
        }
        else
        {
          // An EMPTY MOTX entry means the material simply has NO texture in that slot.
          //
          // Substituting the green shanecube placeholder is reasonable for a missing FIRST texture (it makes
          // an authoring error obvious), but it is wrong for the SECOND: Env/EnvMetal ADD that layer
          // (out = lighting(tex) + tex_2 * tex * tex.a), so the placeholder's bright-green grid got added on
          // top of the surface -- and because the env coordinate is a reflection vector, the green swam
          // across the model as the camera turned. Stormwind's SW_Harbor_Docks.wmo does exactly this: the
          // docked ship's 4 EnvMetal materials (front/rear/blade/metalhull) declare an EMPTY texture2, while
          // the standalone Transport_Icebreaker_ship_nomasts.wmo names a real env map (WR_ENV.BLP) and
          // renders correctly. Black is the neutral element for an additive layer, so an absent env map now
          // contributes nothing. Note this path never logged "file not found" -- the name is empty, not
          // missing -- which is why it hid for so long. [2026-07-30]
          is_empty = ofs_or_id >= texbuf.size() || !texbuf[ofs_or_id];
          texture = is_empty ? std::string(is_second_texture ? "tileset/generic/black.blp"
                                                             : "textures/shanecube.blp")
                             : std::string(&texbuf[ofs_or_id]);

          // Custom WMOs (Turtle world/custom/kttown/kttown.wmo) reference textures by BARE filename
          // (window.blp, floor.blp, wall3.blp) with NO directory. A bare name collides with same-named
          // root textures shipped by other patches, so noggit's patch load-order resolves them to the WRONG
          // image ("wrong textures" on the building). Resolve a directory-less name from the WMO's OWN folder
          // first (world/custom/kttown/window.blp) -- a unique, collision-free path -- and only fall back to
          // the bare name if no co-located texture exists. Standard full-path MOTX entries are unaffected.
          if (_file_key.hasFilepath() && texture.find('/') == std::string::npos && texture.find('\\') == std::string::npos)
          {
            std::string const wmo_path = _file_key.filepath();
            auto const slash = wmo_path.find_last_of("/\\");
            if (slash != std::string::npos)
            {
              std::string const co_located = wmo_path.substr(0, slash + 1) + texture;
              if (client_data->exists(co_located))
              {
                texture = co_located;
              }
            }
          }
        }

        // Empty entries resolve to a DIFFERENT substitute depending on the slot, so fold the slot into the
        // cache key for them -- otherwise a first-slot shanecube could be handed back for a second slot
        // (or vice versa) whenever both reference the same empty offset.
        std::uint32_t const cache_key = (is_empty && is_second_texture) ? (ofs_or_id | 0x80000000u) : ofs_or_id;

        auto const mapping
          (texture_offset_to_inmem_index.emplace(cache_key, static_cast<std::uint32_t>(textures.size())));

        if (mapping.second)
        {
          textures.emplace_back(texture, _context);
        }
        return mapping.first->second;
      }
    );

  // ---------------------------------------------------------------- chunk loop
  while (f.getPos() + 8 <= f.getSize())
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);
    std::size_t const chunk_end = f.getPos() + size;
    if (chunk_end > f.getSize())
    {
      LogError << "WMO \"" << _file_key.stringRepr() << "\": chunk runs past the file; stopping." << std::endl;
      break;
    }

    switch (fourcc)
    {
      case 'MOTX':
      {
        texbuf.assign(f.getPointer(), f.getPointer() + size);
        have_motx = true;
        break;
      }
      case 'MOMT':
      {
        std::size_t const num_materials (size / 0x40);
        materials.resize (num_materials);
        material_env_texture_missing.assign (num_materials, 0u);

        for (size_t i(0); i < num_materials; ++i)
        {
          f.read(&materials[i], sizeof(WMOMaterial));

          uint32_t shader = materials[i].shader;
          bool use_second_texture = (shader == 6 || shader == 5 || shader == 3);

          materials[i].texture1 = load_texture(materials[i].texture_offset_1, false);
          if (use_second_texture)
          {
            bool const second_missing = have_motx
              ? (materials[i].texture_offset_2 >= texbuf.size() || !texbuf[materials[i].texture_offset_2])
              : materials[i].texture_offset_2 == 0;
            material_env_texture_missing[i] = second_missing ? 1u : 0u;
            materials[i].texture2 = load_texture(materials[i].texture_offset_2, true);
          }
        }
        break;
      }
      case 'MOGN':
      {
        groupnames = reinterpret_cast<char const*> (f.getPointer ());
        break;
      }
      case 'MOGI':
      {
        std::size_t const group_count = std::min<std::size_t>(nGroups, size / 32);
        groups.reserve(group_count);
        for (int i (0); i < static_cast<int>(group_count); ++i) {
          groups.emplace_back (this, &f, i, groupnames);
        }
        break;
      }
      case 'MOSB':
      {
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
            if (client_data->exists(path))
            {
              skybox = scoped_model_reference(path, _context);
            }
          }
        }
        break;
      }
      case 'MOSI':
      {
        if (size >= 4)
        {
          f.read(&skybox_file_id, 4);
        }
        break;
      }
      case 'MOPV':
      {
        // Portal polygon corners. Same X/Z-up -> Y-up swap the rest of the WMO geometry uses.
        _portal_vertices.reserve(size / 12);
        for (size_t i (0); i < size / 12; ++i)
        {
          f.read (ff, 12);
          _portal_vertices.push_back(glm::vec3(ff[0], ff[2], -ff[1]));
        }
        break;
      }
      case 'MOPT':
      {
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
        break;
      }
      case 'MOPR':
      {
        _portal_refs.resize(size / sizeof(WMOPR));
        if (size)
        {
          f.read (_portal_refs.data(), size);
        }
        break;
      }
      case 'MNLD':
      {
        // Shadowlands+ dynamic lights (wowdev WMO/MNLD, 184 bytes each): int type (0 point, 1 spot), int
        // lightIndex, int flags (0x1 blend colours, 0x2 shadow), int doodadSet, CImVector innerColor, C3Vector
        // position, C3Vector rotation, float attenStart, attenEnd, intensity, CImVector outerColor, float
        // blendStart, blendEnd, flicker, cookie fdid, falloff, cone angles, half-float scale / multiplier.
        // Modern groups reference these through MNLR instead of MOLT through MOLR: Karazhan Crypts
        // (classic_md_crypt_d.wmo, Classic Era 1.15.9) carries 283 of them and 389 MNLR refs against 0 MOLR
        // refs, with a black MOHD ambient and MOCV averaging 2/255 -- ALL of its interior light. Without
        // them every room drew black. docs/client_re/42 sec 15.
        constexpr std::size_t record_size = 184;
        std::size_t const count = size / record_size;
        new_lights.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
          char rec[record_size];
          f.read(rec, record_size);
          WMOLight l;
          std::memcpy(&l.light_type, rec + 0, 4);
          std::memcpy(&l.flags, rec + 8, 4);
          std::memcpy(&l.doodad_set, rec + 12, 4);
          std::memcpy(&l.color, rec + 16, 4);
          glm::vec3 p;
          std::memcpy(&p, rec + 20, 12);
          float atten_end = 0.f;
          std::memcpy(&atten_end, rec + 48, 4);
          std::memcpy(&l.intensity, rec + 52, 4);
          std::fill(std::begin(l.unk), std::end(l.unk), 0.f);
          l.pos = glm::vec3(p.x, p.z, -p.y); // same axis swap as MOLT / MOVT
          l.r = atten_end;
          l.modern = true;
          float const fr = ((l.color & 0x00ff0000) >> 16) / 255.0f;
          float const fg = ((l.color & 0x0000ff00) >> 8) / 255.0f;
          float const fb = (l.color & 0x000000ff) / 255.0f;
          l.fcolor = glm::vec4(fr, fg, fb, 1.0f);
          new_lights.push_back(l);
        }
        break;
      }
      case 'MOLT':
      {
        // Trust the CHUNK SIZE so a bad MOHD.nLights can't run the read past the chunk. Each MOLT entry is
        // 0x30 bytes; min() is a no-op when the header agrees with the chunk (every valid WMO).
        std::size_t const light_count = std::min<std::size_t> (nLights, size / 0x30);
        lights.reserve(light_count);
        for (size_t i (0); i < light_count; ++i) {
          WMOLight l;
          l.init (&f);
          lights.push_back (l);
        }
        break;
      }
      case 'MODS':
      {
        // Robustness (Turtle world/custom/kt_Farm/ktfarm.wmo, kt_Inn/ktinn.wmo): read the doodad-set count from
        // the CHUNK SIZE, not blindly from MOHD.nDoodadSets. Those custom WMOs carry a MOHD nDoodadSets (6, 4)
        // that OVERRUNS their actual 32-byte (1-set) MODS chunk -> reading nDoodadSets*32 bytes ran past the
        // chunk and DESYNCED every following chunk. Clamp to what the chunk holds.
        std::size_t const set_count = std::min<std::size_t> (nDoodadSets, size / 32);
        doodadsets.reserve(set_count);
        for (size_t i (0); i < set_count; ++i) {
          WMODoodadSet dds;
          f.read (&dds, 32);
          doodadsets.push_back (dds);
        }
        break;
      }
      case 'MODN':
      {
        if (size)
        {
          ddnames = reinterpret_cast<char const*> (f.getPointer ());
        }
        break;
      }
      case 'MODD':
      {
        // Guard a corrupt MODD chunk size. `size / 0x28` is the doodad count; a broken/custom (fuckported) WMO
        // -- e.g. Turtle's world/wmo/playerhousing/human/humanlevelonetest.wmo -- can carry a bogus MODD size of
        // over a gigabyte, which made this create TENS OF MILLIONS of wmo_doodad_instance (34.5M observed, ~7GB,
        // froze the client at load). Doodad refs (MODR) are uint16, so nothing past index 65535 is ever
        // referenceable -- clamp there. reserve() must use the clamped count too or it alone allocates GBs.
        modd_pos = f.getPos();
        doodad_count = size / 0x28;
        constexpr std::size_t MAX_WMO_MODD_DOODADS = 65536;
        if (doodad_count > MAX_WMO_MODD_DOODADS)
        {
          LogError << "WMO \"" << _file_key.stringRepr() << "\" MODD claims " << doodad_count << " doodads "
                   << "(corrupt chunk); clamping to " << MAX_WMO_MODD_DOODADS << " to avoid OOM/freeze." << std::endl;
          doodad_count = MAX_WMO_MODD_DOODADS;
        }
        break;
      }
      case 'MODI':
      {
        doodad_ids.resize(size / 4);
        if (size >= 4)
        {
          f.read(doodad_ids.data(), (size / 4) * 4);
        }
        break;
      }
      case 'GFID':
      {
        _group_file_ids.resize(size / 4);
        if (size >= 4)
        {
          f.read(_group_file_ids.data(), (size / 4) * 4);
        }
        break;
      }
      case 'MFOG':
      {
        int nfogs = size / 0x30;
        // [2026-07-24] Defensive: assert() is a NO-OP in Release, so a misaligned/garbage MFOG chunk silently made
        // nfogs balloon to ~28.7 MILLION for a Stormwind WMO -- its per-fog loop then cost ~3.4s/frame in
        // World::collect_camera_fog (0 fps in Stormwind). A real WMO has at most a few dozen fogs; reject an
        // absurd count so we neither allocate 1.4 GB nor iterate garbage every frame.
        if (nfogs < 0 || nfogs > 4096)
        {
          LogError << "WMO: invalid MFOG chunk (absurd nfogs=" << nfogs << ") -- skipping placed fog" << std::endl;
          nfogs = 0;
        }
        fogs.reserve(nfogs);

        for (int i (0); i < nfogs; ++i)
        {
          WMOFog fog;
          fog.init (&f);
          fogs.push_back (std::move(fog));
        }
        break;
      }
      default:
        // MOVV / MOVB (visible-block data), MCVP, MOUV, MDAL, MAVG/MAVD, MOLP/MOLS/MNLD ...: not consumed
        break;
    }

    f.seek (chunk_end);
  }

  // - MODD (resolved after the loop: the name source is MODN before it or MODI after it) --------------
  if (doodad_count && !ddnames && doodad_ids.empty())
  {
    LogError << "WMO \"" << _file_key.stringRepr() << "\" has doodads but neither MODN nor MODI; ignoring them." << std::endl;
    doodad_count = 0;
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

    f.seek (modd_pos + i * 0x28);
    f.read (&x, sizeof (x));

    if (!doodad_ids.empty())
    {
      // modern: name_offset is an INDEX into MODI. An unresolvable entry still gets an instance so the
      // groups' MODR indices stay aligned (it fails to load and is skipped by the renderer).
      std::uint32_t const fdid = x.name_offset < doodad_ids.size() ? doodad_ids[x.name_offset] : 0u;
      BlizzardArchive::Listfile::FileKey key(fdid);
      key.deduceOtherComponent(client_data->listfile());
      modelis.emplace_back(key, &f, _context);
    }
    else
    {
      modelis.emplace_back(BlizzardArchive::Listfile::FileKey(std::string(ddnames + x.name_offset)), &f, _context);
    }
    model_nearest_light_vector.emplace_back();
  }

  if (skybox_file_id && !skybox)
  {
    BlizzardArchive::Listfile::FileKey key(skybox_file_id);
    key.deduceOtherComponent(client_data->listfile());
    skybox = scoped_model_reference(key, _context);
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

std::vector<float> WMO::intersect (math::ray const& ray, bool do_exterior, float max_dist) const
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

    group.intersect (ray, &results, max_dist);
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
  , _new_light_refs(other._new_light_refs)
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
  // open group file: "<root>_NNN.wmo" (WotLK) or the GFID fileDataID (modern CASC roots)
  auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
  BlizzardArchive::Listfile::FileKey group_key;
  if (static_cast<std::size_t>(num) < wmo->_group_file_ids.size() && wmo->_group_file_ids[num])
  {
    group_key = BlizzardArchive::Listfile::FileKey(wmo->_group_file_ids[num]);
    group_key.deduceOtherComponent(client_data->listfile());
  }
  else if (wmo->file_key().hasFilepath())
  {
    std::stringstream curNum;
    curNum << "_" << std::setw (3) << std::setfill ('0') << num;

    std::string fname = wmo->file_key().filepath();
    auto const ext = fname.find (".wmo");
    fname.insert (ext == std::string::npos ? fname.size() : ext, curNum.str ());
    group_key = BlizzardArchive::Listfile::FileKey(fname);
  }
  else
  {
    LogError << "Error loading WMO group " << num << " of \"" << wmo->file_key().stringRepr() << "\": no GFID and no path." << std::endl;
    return;
  }
  std::string const fname = group_key.stringRepr();

  BlizzardArchive::ClientFile f(group_key, client_data);
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
  uint32_t mogp_size = 0;
  f.read (&mogp_size, 4);

  assert (fourcc == 'MOGP');

  std::size_t const mogp_end = std::min<std::size_t>(f.getPos() + mogp_size, f.getSize());

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

  // - MOGP sub-chunks: DISPATCH by name (2026-09-15). The fixed WotLK order (MOPY MOVI MOVT MONR MOTV MOBA
  // then flag-gated MOLR/MODR/MOBN/MOBR/MPB*/MOCV/MLIQ/MORI/MORB/MOTV/MOCV) breaks on modern (CASC) groups,
  // which insert MOGX / MOBS / MFVR / MDAL and replace MOPY by MPY2 (u16 flags, u16 material); a mismatch
  // used to be "handled" by reading the wrong chunk as the expected one (MPY2 bytes as MOVI indices ->
  // garbage index buffer -> heap corruption). Unknown chunks are skipped; the semantics of repeated
  // chunks (second MOTV / second MOCV) follow the header flags exactly as before.
  std::vector<wmo_triangle_material_info> mopy_entries;
  int motv_seen = 0;
  int mocv_seen = 0;

  while (f.getPos() + 8 <= mogp_end)
  {
    f.read (&fourcc, 4);
    f.read (&size, 4);
    std::size_t const chunk_end = f.getPos() + size;
    if (chunk_end > mogp_end)
    {
      LogError << "WMO group \"" << fname << "\": chunk runs past MOGP; stopping." << std::endl;
      break;
    }

    switch (fourcc)
    {
      case 'MOPY':
      {
        // PER-TRIANGLE MATERIALS. This chunk used to be SKIPPED outright (`f.seekRelative(size)`), which
        // left `_material_infos` permanently EMPTY -- so every `tri < _material_infos.size()` test in
        // this file silently evaluated false: the collidable/detail filter never applied, and the
        // footstep ground-type lookup could never find a face's material (2026-08-27, the "grass
        // footsteps on Stormwind stone" report -- it fell through to the terrain BENEATH the city).
        // Entry = {flags:u8, material:u8}; material 0xFF marks a collision-only (invisible) face.
        mopy_entries.resize(size / sizeof(wmo_triangle_material_info));
        if (!mopy_entries.empty())
        {
          f.read(mopy_entries.data(), mopy_entries.size() * sizeof(wmo_triangle_material_info));
        }
        break;
      }
      case 'MPY2':
      {
        // modern replacement of MOPY: {u16 flags, u16 material} per triangle
        std::size_t const count = size / 4;
        mopy_entries.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
          std::uint16_t flags16 = 0, material16 = 0;
          f.read(&flags16, 2);
          f.read(&material16, 2);
          std::uint8_t const flags8 = static_cast<std::uint8_t>(flags16 & 0xFF);
          std::memcpy(&mopy_entries[i].flags, &flags8, 1);
          mopy_entries[i].texture = static_cast<std::uint8_t>(std::min<std::uint16_t>(material16, 0xFF));
        }
        break;
      }
      case 'MOVI':
      {
        _indices.resize (size / sizeof (uint16_t));
        f.read (_indices.data (), size);
        break;
      }
      case 'MOVT':
      {
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
        break;
      }
      case 'MONR':
      {
        _normals.resize (size / sizeof (::glm::vec3));
        f.read (_normals.data(), size);

        for (auto& n : _normals)
        {
          n = {n.x, n.z, -n.y};
        }
        break;
      }
      case 'MOTV':
      {
        // first set = the material uvs; a second set (has_two_motv) feeds the env/blend layers
        if (motv_seen == 0)
        {
          _texcoords.resize (size / sizeof (glm::vec2));
          f.read (_texcoords.data (), size);
        }
        else if (motv_seen == 1)
        {
          _texcoords_2.resize(size / sizeof(glm::vec2));
          f.read(_texcoords_2.data(), size);
        }
        ++motv_seen;
        break;
      }
      case 'MOBA':
      {
        _batches.resize (size / sizeof (wmo_batch));
        f.read (_batches.data (), size);
        // Legion+ batches (wowdev SMOBatch): flag 0x2 = flag_use_material_id_large -- the material index is
        // the uint16 at +0x0A (inside the old bounding-box bytes) and `material_id` at +0x17 stays 0. EVERY
        // batch of the Classic Era 1.15.9 SoD WMOs sets it (Karazhan Crypts 172/172, the Scarlet Enclave
        // monastery 308/308), so every surface drew with material 0's texture: "missing textures" inside the
        // crypt, wrong textures on the monastery. docs/client_re/42 sec 15.
        for (auto& batch : _batches)
        {
          if (batch.flags & 0x2)
          {
            uint16_t large = 0;
            std::memcpy(&large, &batch.unused[10], sizeof(large));
            if (large < 256)
            {
              batch.texture = static_cast<uint8_t>(large);
            }
            else
            {
              LogError << "WMO group \"" << fname << "\": batch material id " << large
                       << " does not fit the 8-bit material index; keeping " << int(batch.texture) << std::endl;
            }
          }
        }
        // NOTE: initRenderBatches() is deferred to the END of load() so it runs after MOCV is parsed
        // (MOCV comes after MOBA in the file).
        break;
      }
      case 'MNLR':
      {
        // Modern light refs: u16 indices into the root's MNLD list (the MOLR of Shadowlands+ groups).
        std::size_t const count = size / sizeof(uint16_t);
        _new_light_refs.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
          uint16_t ref = 0;
          f.read(&ref, sizeof(ref));
          _new_light_refs[i] = static_cast<int16_t>(std::min<uint16_t>(ref, 0x7fffu));
        }
        break;
      }
      case 'MOLR':
      {
        // Per-group light references: indices into the root MOLT list naming which lights illuminate
        // THIS group (the client's per-room lighting). Used by WorldRender to scope the point-light
        // UBO per interior group instead of the global nearest-16 pool.
        _light_refs.resize (size / sizeof (int16_t));
        f.read (_light_refs.data (), _light_refs.size () * sizeof (int16_t));
        break;
      }
      case 'MODR':
      {
        // Guard a corrupt MODR chunk size: a huge count would allocate gigabytes and feed the doodad
        // explosion in WMO::doodads_per_group. A group realistically has at most a few thousand refs.
        std::uint32_t count = size / sizeof (int16_t);
        constexpr std::uint32_t MAX_DOODAD_REFS = 1000000u;
        if (count > MAX_DOODAD_REFS)
        {
          LogError << "WMO group MODR ref count " << count << " is absurd; clamping to " << MAX_DOODAD_REFS
                   << " (corrupt chunk)." << std::endl;
          count = MAX_DOODAD_REFS;
        }
        _doodad_ref.resize (count);
        f.read (_doodad_ref.data (), count * sizeof (int16_t));
        break;
      }
      case 'MOCV':
      {
        // lighting colours first (has_vertex_color), the texture-blend set after (use_mocv2_for_texture_blending)
        bool const lighting_set = header.flags.has_vertex_color && mocv_seen == 0;
        if (lighting_set)
        {
          load_mocv(f, size);
        }
        else if (header.flags.use_mocv2_for_texture_blending)
        {
          std::vector<CImVector> mocv_2(size / sizeof(CImVector));
          f.read(mocv_2.data(), size);
          _blend_alphas.resize(mocv_2.size());

          for (int i = 0; i < mocv_2.size(); ++i)
          {
            float alpha = static_cast<float>(mocv_2[i].a) / 255.f;

            // the second mocv is texture-blend ONLY -> its own stream, so it no longer clobbers .w
            // (which carries the portal-openness doorway fade for indoor groups)
            _blend_alphas[i] = alpha;
            if (!header.flags.has_vertex_color)
            {
              // no lighting MOCV: keep a placeholder colour (rgb unused; HasMOCV stays off)
              _vertex_colors.emplace_back(0.f, 0.f, 0.f, alpha);
            }
          }
        }
        ++mocv_seen;
        break;
      }
      case 'MLIQ':
      {
        if (size < 0x1E)
        {
          break;
        }
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
            , (bool)header.flags.indoor // city channel (canals) vs open-air pool (see wmo_liquid.hpp)
            // [2026-09-09] exterior water authored on the LiquidType.dbc path (stock WotLK Stormwind
            // canals: EXTERIOR groups, groupLiquid 5) -> the client's flat river-deep colour instead of
            // the open-air-pool river blend that turned them olive green (see wmo_liquid.hpp)
            , (bool)wmo->flags.use_liquid_type_dbc_id && !interior_water && !header.flags.indoor
            , &_vertices // group mesh for the FLOATLIQ geometric clip (phantom flat-sheet tiles)
        );
        break;
      }
      default:
        // MOBN / MOBR (bsp), MPBV / MPBP / MPBI / MPBG, MORI / MORB, MOGX, MOBS, MFVR, MDAL, MOLV, MOPL,
        // MOTA, MOLM / MOLD ...: not consumed
        break;
    }

    f.seek (chunk_end);
  }

  // SAFETY: only adopt the table when it matches the triangle count exactly. A short/mismatched
  // MOPY combined with the collidable filter could otherwise make real FLOOR faces non-collidable
  // and drop the player through the world -- leaving it empty preserves the old behaviour.
  if (!mopy_entries.empty() && mopy_entries.size() == _indices.size() / 3)
  {
    _material_infos = std::move(mopy_entries);
  }
  else if (!mopy_entries.empty())
  {
    LogError << "WMO group: MOPY has " << mopy_entries.size() << " entries for "
             << (_indices.size() / 3) << " triangles -- ignoring (per-face materials unavailable)"
             << std::endl;
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
    // MOHD 0x8 (Cata+ exports -- every SoD WMO, e.g. Karazhan Crypts): the client skips FixColorVertexAlpha
    // except for ONE step -- the vertices of the interior and exterior batches get alpha 0 / 255 and only the
    // transition batch keeps its authored alpha (wowdev CMapObjGroup::FixColorVertexAlpha; the face path in
    // load_mocv already applies it). The RAW alpha of these files is 255 on every deep-interior vertex
    // (crypt: 16 of 19 groups all-255, the entrance group near 0 -- the inverse of WotLK files), so sampling
    // it as the doorway spill lit every prop inside the black rooms with the outdoor light.
    // docs/client_re/42 sec 16.
    std::size_t const interior_vertex_start = header.transparency_batches_count > 0
      ? static_cast<std::size_t>(_batches[header.transparency_batches_count - 1].vertex_end) + 1u : 0u;
    for (std::size_t i = 0; i < _vertices.size(); ++i)
    {
      _ground_colors[i] = glm::u8vec3(static_cast<std::uint8_t>(glm::clamp(eff_amb.x + _vertex_colors[i].x, 0.f, 1.f) * 255.f)
                                    , static_cast<std::uint8_t>(glm::clamp(eff_amb.y + _vertex_colors[i].y, 0.f, 1.f) * 255.f)
                                    , static_cast<std::uint8_t>(glm::clamp(eff_amb.z + _vertex_colors[i].z, 0.f, 1.f) * 255.f));
      float pa = (i < _mocv_pristine_alpha.size()) ? _mocv_pristine_alpha[i] : 0.f;
      if (wmo->flags.do_not_fix_vertex_color_alpha && i >= interior_vertex_start)
      {
        pa = 0.f; // interior batch of a do-not-fix file: the client's fixed alpha is 0 (this group is TRUE indoor)
      }
      _ground_alphas[i] = static_cast<std::uint8_t>(glm::clamp(pa, 0.f, 1.f) * 255.f + 0.5f);
    }
  }

  // Deferred from just after the MOBA read: build render batches now that MOCV has been parsed.
  _renderer.initRenderBatches();
}

bool WMOGroup::sample_ground_color(glm::vec3 const& local_pos, glm::vec3* out, float* out_alpha,
                                   float* out_floor_y) const
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
  float best_y = local_pos.y - 12.0f;
  bool found = false;

  // One floor-triangle test (the client's SampleGroundColor barycentric interpolation). Kept as a lambda so
  // it can be driven from either the collision-grid cell (fast) or the full triangle list (fallback).
  auto const test_tri = [&](std::size_t i)
  {
    if (i + 2 >= _indices.size()) { return; }
    std::uint16_t const ia = _indices[i], ib = _indices[i + 1], ic = _indices[i + 2];
    if (ia >= _vertices.size() || ib >= _vertices.size() || ic >= _vertices.size())
    {
      return;
    }
    glm::vec3 const& a = _vertices[ia];
    glm::vec3 const& b = _vertices[ib];
    glm::vec3 const& c = _vertices[ic];

    // Horizontal (xz) barycentric test -- the client projects the hit onto the face's dominant plane,
    // which for a floor face is the horizontal one.
    float const den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::abs(den) < 1e-6f)
    {
      return; // vertical face (wall): no horizontal footprint to stand on
    }
    float const l0 = ((b.z - c.z) * (local_pos.x - c.x) + (c.x - b.x) * (local_pos.z - c.z)) / den;
    float const l1 = ((c.z - a.z) * (local_pos.x - c.x) + (a.x - c.x) * (local_pos.z - c.z)) / den;
    float const l2 = 1.f - l0 - l1;
    if (l0 < -0.001f || l1 < -0.001f || l2 < -0.001f)
    {
      return;
    }

    float const y = l0 * a.y + l1 * b.y + l2 * c.y;
    if (y > top || y <= best_y)
    {
      return; // above the entity's feet, below the 12-unit reach, or below a closer floor already found
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
  };

  // [perf 2026-08-18] Query only the floor triangles in the collision grid's cell under local_pos.xz instead
  // of scanning EVERY triangle of the group. This was the dominant interior-lighting cost (sample_ground_color
  // ran O(all group triangles) per unit per frame -- BucketInterior ~20ms in a WMO crowd, ~4ms after the
  // per-frame cache; this finishes it). The grid buckets each COLLIDABLE triangle into every cell its XZ AABB
  // overlaps, so the single containing cell holds every candidate whose footprint could contain the point --
  // and the client itself samples the collidable floor (a downward collision ray), so this is client-faithful.
  // Falls back to the full scan only when the grid is unavailable (degenerate group).
  if (!_collision_grid_built)
  {
    buildCollisionGrid();
  }
  if (_collision_grid_cell > 0.0f)
  {
    float const inv = 1.0f / _collision_grid_cell;
    int const cx = std::clamp(static_cast<int>((local_pos.x - _collision_grid_origin.x) * inv),
                              0, _collision_grid_nx - 1);
    int const cz = std::clamp(static_cast<int>((local_pos.z - _collision_grid_origin.y) * inv),
                              0, _collision_grid_nz - 1);
    for (std::uint32_t const i : _collision_grid[static_cast<std::size_t>(cz)
                                                 * static_cast<std::size_t>(_collision_grid_nx) + cx])
    {
      test_tri(i);
    }
  }
  else
  {
    for (std::size_t i = 0; i + 2 < _indices.size(); i += 3)
    {
      test_tri(i);
    }
  }

  if (found && out_floor_y)
  {
    *out_floor_y = best_y;
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
  // NOTE: the `use_mocv2_for_texture_blending` bail was REMOVED (2026-08-12). It existed only
  // because the mocv2 blend alpha shared .w; it now has its own stream (_blend_alphas), so these
  // groups can get their doorway fade like every other indoor group. That bail was the reason
  // Ascension interiors hard-cut at the door while stock blended (branch-visualiser: no BLUE).
  if (!header.flags.indoor
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


std::optional<float> WMOGroup::liquidHeightAtLocal(glm::vec3 const& p, int* out_liquid_id) const
{
  if (!lq)
  {
    return std::nullopt;
  }
  if (p.x < VertexBoxMin.x - 1.0f || p.x > VertexBoxMax.x + 1.0f
      || p.y < VertexBoxMin.y - 1.0f || p.y > VertexBoxMax.y + 1.0f
      || p.z < VertexBoxMin.z - 1.0f || p.z > VertexBoxMax.z + 1.0f)
  {
    return std::nullopt;
  }
  auto const h = lq->heightAtLocal(p);
  if (h && out_liquid_id)
  {
    *out_liquid_id = lq->liquid_id();
  }
  return h;
}

// [perf 2026-08-17] Build the lazy 2D (XZ) collision grid over this group's COLLIDABLE triangles. See
// WMO.h. Called once on the first reach-limited physics probe; on any degenerate/absurd bounds it leaves
// _collision_grid_cell == 0 so intersect() transparently falls back to the full batch walk.
void WMOGroup::buildCollisionGrid() const
{
  _collision_grid_built = true;
  _collision_grid_cell = 0.0f; // 0 => unavailable; intersect() uses the batch walk instead

  if (_indices.size() < 3 || _vertices.empty())
  {
    return;
  }

  glm::vec3 const mn(glm::min(VertexBoxMin, VertexBoxMax));
  glm::vec3 const mx(glm::max(VertexBoxMin, VertexBoxMax));
  float const sx = mx.x - mn.x;
  float const sz = mx.z - mn.z;
  if (!(sx > 0.001f && sz > 0.001f && sx < 1.0e7f && sz < 1.0e7f))
  {
    return; // degenerate / absurd bounds -> keep the batch-walk fallback
  }

  float constexpr cell = 4.0f; // ~4yd cells: short physics rays (<=~2yd) touch ~1-4 cells
  int const nx = static_cast<int>(sx / cell) + 1;
  int const nz = static_cast<int>(sz / cell) + 1;
  if (static_cast<long long>(nx) * static_cast<long long>(nz) > 2000000LL)
  {
    return; // pathological extent -> fall back rather than allocate a giant grid
  }

  _collision_grid_origin = glm::vec2(mn.x, mn.z);
  _collision_grid_cell = cell;
  _collision_grid_nx = nx;
  _collision_grid_nz = nz;
  _collision_grid.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz), {});

  std::size_t const tri_count = _indices.size() / 3;
  _collision_grid_visit.assign(tri_count, 0u);
  _collision_grid_query = 0u;

  float const inv = 1.0f / cell;
  for (std::size_t tri = 0; tri < tri_count; ++tri)
  {
    // Only COLLIDABLE faces (same rule the physics batch walk applies) so the query needs no recheck.
    if (tri < _material_infos.size()
        && !const_cast<wmo_triangle_material_info&>(_material_infos[tri]).isCollidable())
    {
      continue;
    }
    std::size_t const i = tri * 3;
    glm::vec3 const& a = _vertices[_indices[i + 0]];
    glm::vec3 const& b = _vertices[_indices[i + 1]];
    glm::vec3 const& c = _vertices[_indices[i + 2]];
    int cx0 = static_cast<int>((std::min({a.x, b.x, c.x}) - mn.x) * inv);
    int cx1 = static_cast<int>((std::max({a.x, b.x, c.x}) - mn.x) * inv);
    int cz0 = static_cast<int>((std::min({a.z, b.z, c.z}) - mn.z) * inv);
    int cz1 = static_cast<int>((std::max({a.z, b.z, c.z}) - mn.z) * inv);
    cx0 = std::clamp(cx0, 0, nx - 1); cx1 = std::clamp(cx1, 0, nx - 1);
    cz0 = std::clamp(cz0, 0, nz - 1); cz1 = std::clamp(cz1, 0, nz - 1);
    for (int cz = cz0; cz <= cz1; ++cz)
    {
      for (int cx = cx0; cx <= cx1; ++cx)
      {
        _collision_grid[static_cast<std::size_t>(cz) * static_cast<std::size_t>(nx) + cx]
          .push_back(static_cast<uint32_t>(i));
      }
    }
  }
}

void WMOGroup::intersect (math::ray const& ray, std::vector<float>* results, float max_dist) const
{
  if (!ray.intersect_bounds (VertexBoxMin, VertexBoxMax))
  {
    return;
  }

  // Short physics probes (game mode): skip groups farther than max_dist from the probe origin.
  // Without this every probe ray triangle-walks EVERY group whose box the infinite ray crosses --
  // in a city WMO that's the whole city per ray, the game-mode framerate collapse. 0 = unlimited.
  if (max_dist > 0.0f)
  {
    glm::vec3 const closest(glm::clamp(ray.origin(), VertexBoxMin, VertexBoxMax));
    if (glm::distance(closest, ray.origin()) > max_dist)
    {
      return;
    }
  }

  // [perf 2026-08-17] Physics probes: use the lazy collision grid so a SHORT ray tests only the triangles
  // in the few cells its span crosses, instead of every triangle of every near batch (measured ~15ms/frame
  // -- the game-view bottleneck). Conservative (a triangle is bucketed into every cell its XZ AABB overlaps,
  // and we gather the ray-span AABB + a 1-cell margin), so it can never miss a triangle the batch walk would
  // hit -> identical collision. Falls through to the batch walk below only if the grid is unavailable.
  if (max_dist > 0.0f)
  {
    if (!_collision_grid_built)
    {
      buildCollisionGrid();
    }
    if (_collision_grid_cell > 0.0f)
    {
      glm::vec3 const o(ray.origin());
      glm::vec3 const e(ray.position(max_dist)); // ray reach endpoint (origin + dir*max_dist)
      float const inv = 1.0f / _collision_grid_cell;
      auto const cell_x = [&](float x)
      { return std::clamp(static_cast<int>((x - _collision_grid_origin.x) * inv), 0, _collision_grid_nx - 1); };
      auto const cell_z = [&](float z)
      { return std::clamp(static_cast<int>((z - _collision_grid_origin.y) * inv), 0, _collision_grid_nz - 1); };
      int const cx0 = cell_x(std::min(o.x, e.x) - _collision_grid_cell);
      int const cx1 = cell_x(std::max(o.x, e.x) + _collision_grid_cell);
      int const cz0 = cell_z(std::min(o.z, e.z) - _collision_grid_cell);
      int const cz1 = cell_z(std::max(o.z, e.z) + _collision_grid_cell);
      ++_collision_grid_query;
      std::uint32_t const q = _collision_grid_query;
      for (int cz = cz0; cz <= cz1; ++cz)
      {
        for (int cx = cx0; cx <= cx1; ++cx)
        {
          for (uint32_t const i : _collision_grid[static_cast<std::size_t>(cz) * static_cast<std::size_t>(_collision_grid_nx) + cx])
          {
            std::uint32_t const tri = i / 3u;
            if (tri < _collision_grid_visit.size())
            {
              if (_collision_grid_visit[tri] == q) { continue; } // already tested this query (spans cells)
              _collision_grid_visit[tri] = q;
            }
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
      return;
    }
  }

  // Lazy per-batch AABBs (see WMO.h): computed once from the converted vertices, then every
  // reach-limited probe rejects whole batches by distance before touching a single triangle.
  if (max_dist > 0.0f && !_batch_bounds_computed)
  {
    _batch_bounds.reserve(_batches.size());
    for (auto const& batch : _batches)
    {
      glm::vec3 bmin(std::numeric_limits<float>::max());
      glm::vec3 bmax(std::numeric_limits<float>::lowest());
      for (size_t i (batch.index_start); i < batch.index_start + batch.index_count; ++i)
      {
        glm::vec3 const& v = _vertices[_indices[i]];
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
      }
      _batch_bounds.emplace_back(bmin, bmax);
    }
    _batch_bounds_computed = true;
  }

  //! \todo Also allow clicking on doodads and liquids.
  std::size_t batch_index = 0;
  for (auto&& batch : _batches)
  {
    std::size_t const bi = batch_index++;
    if (max_dist > 0.0f)
    {
      if (batch.index_count == 0)
      {
        continue;
      }
      auto const& bounds = _batch_bounds[bi];
      glm::vec3 const closest(glm::clamp(ray.origin(), bounds.first, bounds.second));
      if (glm::distance(closest, ray.origin()) > max_dist)
      {
        continue;
      }
    }
    for (size_t i (batch.index_start); i < batch.index_start + batch.index_count; i += 3)
    {
      // PHYSICS probes (max_dist > 0) use the CLIENT's face-collision rule: a face collides when
      // MOPY flags say collision, or render-without-detail (isCollidable). DETAIL faces (grates,
      // vines, door decor -- e.g. the Blackrock doorway pieces) are walk-through in the game;
      // colliding with them made invisible walls. Editor picking (max_dist == 0) keeps every face.
      if (max_dist > 0.0f)
      {
        size_t const tri = i / 3;
        if (tri < _material_infos.size()
            && !const_cast<wmo_triangle_material_info&>(_material_infos[tri]).isCollidable())
        {
          continue;
        }
      }
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

void WMOGroup::groundQuery(math::ray const& ray, float reach, wmo_ground_query& io) const
{
  // 3.3.5a CWorld ground-type cast, per group (FUN_007c25d0 -> FUN_007c1dc0 -> BSP walk
  // FUN_007ca600 / per-face FUN_007c6600). RE'd 2026-09-09 for "Stormwind streets fall back to
  // the ADT texture": beside the canals the character stands on a COLLISION-only ghost face
  // (MOPY 0x48, material 0xFF -- Stormwind_284 'canalB' tri 20833) that no render batch holds, so
  // a batch-only walk saw nothing at all. The client's BSP references every face, and its
  // per-face test keeps TWO nearest hits: the type is read from the "typed" one however far
  // below the support it lies (the canal floor, 30 yd down, in that spot).
  if (header.flags.value & 0x410080u) // unreachable / always_draw / 0x400000: never queried
  {
    return;
  }
  if (!ray.intersect_bounds(VertexBoxMin, VertexBoxMax))
  {
    return;
  }
  if (!header.flags.exterior)
  {
    // interior room: only a room the origin is INSIDE can hold the floor under it (FUN_007ae920)
    glm::vec3 const& o = ray.origin();
    if (o.x < VertexBoxMin.x || o.x > VertexBoxMax.x || o.y < VertexBoxMin.y || o.y > VertexBoxMax.y
        || o.z < VertexBoxMin.z || o.z > VertexBoxMax.z)
    {
      return;
    }
  }
  std::size_t const tri_count = std::min(_indices.size() / 3, _material_infos.size());
  for (std::size_t tri = 0; tri < tri_count; ++tri)
  {
    std::uint8_t const fl = static_cast<std::uint8_t>(_material_infos[tri].flags.value);
    if (fl & 0x82u) // the walk's skip mask: 0x02 faces and the 0x80 visited mark
    {
      continue;
    }
    bool const render = (fl & 0x20u) != 0;
    bool const collision = (fl & 0x08u) != 0;
    bool const detail = (fl & 0x04u) != 0;
    bool const for_support = render || collision;
    bool const for_typed = render || (!collision && detail);
    if (!for_support && !for_typed)
    {
      continue;
    }
    std::size_t const i = tri * 3;
    auto const d = ray.intersect_triangle ( _vertices[_indices[i + 0]]
                                          , _vertices[_indices[i + 1]]
                                          , _vertices[_indices[i + 2]]
                                          );
    if (!d || *d < 0.0f || *d > reach)
    {
      continue;
    }
    if (for_support && *d <= io.support_t)
    {
      io.support_t = *d;
    }
    if (for_typed && *d <= io.typed_t)
    {
      io.typed_t = *d;
      std::uint8_t const mat = _material_infos[tri].texture;
      io.typed_ground_type = (mat != 0xff && mat < wmo->materials.size())
                           ? static_cast<int>(wmo->materials[mat].ground_type)
                           : -1;
    }
  }
}

void WMO::groundQuery(math::ray const& ray, float reach, wmo_ground_query& io) const
{
  for (auto const& group : groups)
  {
    group.groundQuery(ray, reach, io);
  }
}

void WMOGroup::drawLiquid ( glm::mat4x4 const& transform
                          , OpenGL::Scoped::use_program& water_shader
                          , Noggit::Rendering::LiquidTextureManager& texture_manager
                          , bool // draw_fog
                          , int animtime
                          , bool translucent
                          )
{
  // draw liquid
  //! \todo  culling for liquid boundingbox or something
  if (lq)
  {
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_TRUE> const depth_test;
    gl.enable(GL_BLEND);

    // Blend is the SAME in both cases -- water's appearance is unchanged. The only difference is the
    // depth write, and that was the actual bug: WMO groups are drawn BEFORE the M2 pass, so a water
    // plane that writes depth occludes every creature standing below it -- they are depth-rejected
    // and never rasterized at all. That is why creatures vanished in the Stormwind canals but
    // reappeared once the camera went under the surface, and why terrain and WMO geometry (both drawn
    // BEFORE the water plane) looked perfectly fine through it. The ADT water pass already gets this
    // right via depth_mask_setter<GL_FALSE>.
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Interior liquid (Molten Core lava) is genuinely opaque and must keep occluding.
    gl.depthMask(translucent ? GL_FALSE : GL_TRUE);

    lq->draw(transform, water_shader, texture_manager, animtime);

    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE); // restore for the opaque passes that follow
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

// [EXIT-CRASH FIX 2026-08-29] Intentionally LEAKED: the three object managers reference each other
// from their element destructors (~WMO -> ModelManager::_, ~Model -> TextureManager::_), and their
// cross-TU static destruction order is unspecified, so whichever died first left the others faulting
// on a destroyed mutex/map during process exit. Never destroying them removes the hazard entirely.
Noggit::AsyncObjectMultimap<WMO>& WMOManager::_ = *new Noggit::AsyncObjectMultimap<WMO>();

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
