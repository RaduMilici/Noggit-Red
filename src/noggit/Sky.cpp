// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/Model.h> // Model
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/Sky.h>
#include <noggit/World.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <opengl/shader.hpp>
#include <ClientFile.hpp>
#include <glm/glm.hpp>

#include <QtCore/QSettings>

#include <algorithm>
#include <string>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cfloat>
#include <limits>
#include <utility>

const float skymul = 36.0f;

namespace
{
  // [client RE 2026-09-09, wow335a.exe] the ZONE-LIGHT POLYGON TABLE, copied verbatim from the client's
  // .data (11 entries @0x00adef58, one per Northrend zone, string pointers @0x00adee48). Each entry is an
  // SVG path ("L x,y ... z") drawn in 33.33-yard map units; FUN_0077ed40 parses it at startup and maps it
  // into world space (see zone_polygon_* constants), FUN_0077eed0 tests the CAMERA position against it every
  // frame and FUN_007ed150 queues the light with value = 50 - signed distance (negative inside), which
  // FUN_007ee6b0 blends BEFORE every positional light with weight = clamp(value / 100). This is how the
  // client puts IceCrownSky under the whole Icecrown zone (Light 1703, LightParams 748) although Light.dbc
  // only holds the zone's positional lights: the tiny-radius, buried lights 914/825/959/862/1847/1703/1796/
  // 1777/1792/1589/1740 exist for this table alone.
  struct ZoneLightPolygonDef { unsigned int map; int light_id; char const* svg_path; };
  ZoneLightPolygonDef const ZONE_LIGHT_POLYGON_DEFS[] = {
    // Light 914: Wintergrasp / Lake Wintergrasp aurora
    {571, 914, "L 415.58442,531.25541 L 387.74026,529.59307 L 385.80086,526.82251 L 385.7316,520.17316 L 375.1342,520.8658 L 374.64936,522.04329 L 373.4026,522.11255 L 372.36364,520.8658 L 365.57576,520.72727 L 365.43723,519.06493 L 348.67533,518.78788 L 343.27273,515.87879 L 334.96104,515.87879 L 333.2987,517.26406 L 319.03031,517.54113 L 243.94805,475.29004 L 225.9394,566.44155 L 266.25109,632.93506 L 350.19914,653.71429 L 415.47711,629.01747 L 416.3857,586.28568 L 417.80507,564.21323 L 415.74891,558.96969 L 420,549.41992 L 420.329,545.35065 L 417.70834,543.36003 L 417.79667,534.77107 z"},
    // Light 825: Borean Tundra aurora
    {571, 825, "L 542.47619,512.69264 L 526.68398,512.27706 L 520.24243,507.15151 L 511.30736,502.02597 L 499.87879,501.47186 L 497.45455,505.48918 L 490.8052,505.48918 L 485.67966,503.34199 L 481.24675,496.7619 L 478.68398,497.03896 L 476.46753,500.50216 L 469.33334,503.54978 L 468.64069,506.38961 L 464.62338,509.85282 L 464.13853,512.76191 L 459.98268,512.7619 L 459.77489,519.48052 L 461.71429,521.28138 L 461.71429,529.59307 L 459.91342,531.53247 L 452.57143,532.50216 L 452.29437,530.83983 L 448.9697,529.87013 L 444.67533,529.87013 L 440.1039,534.58009 L 435.94805,534.71861 L 435.4632,536.45022 L 426.73593,536.24242 L 420.98701,535.68831 L 417.66234,534.99567 L 417.69697,543.34199 L 420.41559,545.36796 L 420.08658,549.54112 L 417.33334,555.30736 L 415.82685,558.9264 L 417.78355,564.20779 L 416.33156,585.52293 L 415.58849,629.51631 L 543.25237,643.44281 L 561.20821,606.2407 L 561.7316,571.5671 L 562.26195,560.9602 L 563.47509,556.54987 L 561.86725,540.03549 L 558.82251,536.24242 L 558.82251,531.94805 L 561.59307,527.37662 L 560.91028,525.07903 L 565.40464,521.28834 L 569.08547,517.3897 L 563.12877,516.65156 L 562.10848,514.5974 L 557.77147,517.15398 L 554.80062,518.02325 L 553.81376,515.66677 L 557.04829,511.88812 L 555.35931,506.73593 L 546.63204,506.5974 z"},
    // Light 959: Grizzly Hills aurora
    {571, 959, "L 658.42424,491.91342 L 652.74459,491.77489 L 651.08225,493.99134 L 650.8052,500.64069 L 646.64935,503.41125 L 641.94924,503.53298 L 638.79671,507.07733 L 628.46448,507.95892 L 626.12391,509.59987 L 625.75179,512.5385 L 610.24718,522.06959 L 609.40905,523.93072 L 601.56576,524.4008 L 595.21146,524.65409 L 589.00962,523.67743 L 588.19575,521.44129 L 585.09363,520.47975 L 584.84817,517.78375 L 568.95883,517.49358 L 565.64112,521.19039 L 560.6738,525.07903 L 561.59307,527.37662 L 558.68398,531.94805 L 558.82251,536.38095 L 561.67134,540.13345 L 563.57305,556.70521 L 562.3599,560.84544 L 561.21229,606.81111 L 561.22741,606.68005 L 590.12987,579.74026 L 602.18182,576.41559 L 602.87446,570.73593 L 603.98269,567.82684 L 607.37662,567.61905 L 608.62338,561.66233 L 611.84417,559.51515 L 615.16883,554.87446 L 617.13524,550.32961 L 621.18672,547.21005 L 624.99578,548.5234 L 632.57923,547.92932 L 638.39647,548.02789 L 642.10114,549.581 L 644.97464,552.13844 L 649.26806,552.72185 L 654.3234,551.1927 L 657.78565,548.10512 L 663.76937,546.6079 L 667.29423,544.86825 L 675.30159,547.4159 L 683.83518,550.88307 L 695.15021,565.17313 L 724.6491,565.13046 L 695.84514,480.29575 z"},
    // Light 862: Howling Fjord aurora
    {571, 862, "L 695.15101,565.24834 L 700.56477,576.68657 L 697.71973,584.91303 L 686.93483,589.08304 L 681.55461,592.64502 L 673.43847,598.56199 L 668.30068,598.00275 L 663.33606,595.08855 L 657.87412,586.26043 L 653.58968,584.60114 L 649.44376,585.40072 L 644.95152,588.55529 L 638.03018,592.4809 L 630.5201,592.597 L 621.9018,590.42739 L 616.20779,587.60173 L 609.14285,581.12554 L 607.30736,575.2381 L 607.16883,567.82684 L 603.84416,567.68831 L 602.87446,570.87446 L 602.04329,576.69263 L 589.85282,579.74026 L 561.01997,606.89803 L 543.41895,643.54753 L 552.78099,681.70681 L 573.81856,706.89785 L 647.85055,713.01615 L 727.44446,698.80992 L 740.88647,644.79209 L 724.61693,565.08148 z"},
    // Light 1847: Howling Fjord east aurora
    {571, 1847, "L 617.05115,550.47504 L 615.15582,554.83316 L 611.74927,559.54239 L 608.63947,561.63364 L 607.37795,567.55746 L 607.25131,575.52354 L 609.15926,581.22073 L 616.15564,587.61752 L 621.81081,590.47067 L 630.45837,592.58916 L 638.02497,592.45064 L 645.11439,588.55576 L 649.45702,585.33672 L 653.57925,584.59931 L 657.94984,586.35119 L 663.32795,595.21626 C 663.32795,595.21626 668.35699,598.07783 668.35699,598.07783 L 673.47556,598.49761 L 681.76985,592.51426 L 686.96945,589.08118 L 697.80117,584.88116 L 700.53681,576.62337 L 695.18955,565.25915 L 683.76721,550.72629 L 675.03046,547.40227 L 667.24876,544.8374 L 663.74426,546.58508 L 657.76307,548.15367 L 654.24669,551.24259 L 649.19952,552.74119 L 644.95435,552.06065 L 642.01606,549.582 L 638.37549,548.08022 L 632.72308,547.91719 L 624.92493,548.38524 L 621.13077,547.06675 z"},
    // Light 1703: Icecrown (IceCrownSky)
    {571, 1703, "L 497.59308,353.80086 L 481.66234,354.07792 L 385.80416,370.15744 L 326.81176,411.95793 L 354.00415,452.83963 L 356.12433,449.18867 L 358.49253,449.47677 L 360.63529,453.37524 L 377.47237,453.15916 L 384.34428,453.53416 L 389.45302,456.39572 L 403.53247,466.83983 L 406.23377,471.75757 L 412.46753,478.19913 L 412.88311,491.5671 L 421.61039,491.70563 L 421.74892,489.76624 L 431.58441,489.62771 L 434.56277,493.36797 L 440.45022,494.12988 L 446.61472,496.48485 L 449.59308,500.77922 L 451.04762,505.76624 L 453.67539,506.92368 L 456.54806,510.72589 L 460.00845,510.65891 L 460.04044,512.74692 L 463.94162,512.72891 L 464.62532,509.93217 L 468.70145,506.31075 L 469.30022,503.61989 L 476.46191,500.43926 L 478.72589,497.06273 L 481.43067,496.77175 L 481.34376,494.96248 L 475.42857,492.46753 L 473.42051,490.20907 L 471.09957,486.71862 L 470.96104,480.62338 L 473.35065,477.64502 L 474.94373,473.76623 L 479.61311,472.9242 L 486.61117,468.15311 L 489.64057,467.93101 L 489.86113,470.65471 L 496.48862,470.67711 L 499.79142,474.54366 L 503.33665,474.46216 L 505.60049,478.70967 L 509.80373,479.18228 L 519.5481,479.3432 L 519.52623,475.00197 L 516.97623,471.45727 L 516.5734,468.91689 L 513.78097,465.71852 L 506.69417,460.54611 L 506.54009,453.78116 L 501.10287,444.25446 L 500.52499,433.89126 L 497.44783,433.29732 L 497.59308,408.31168 z"},
    // Light 1796: Sholazar Basin aurora
    {571, 1796, "L 412.45434,478.09436 L 406.54061,471.96297 L 403.37534,466.73863 L 394.16765,459.68593 L 389.46584,456.25753 L 384.17632,453.51482 L 376.92771,453.22096 L 360.56936,453.41686 L 358.80618,449.40074 L 356.16142,449.20483 L 353.92036,452.92709 L 326.87313,411.98224 L 243.65219,475.1056 L 318.68554,517.07028 L 333.534,517.4789 L 335.00332,515.91164 L 343.23147,516.00959 L 348.7169,518.85026 L 365.36912,518.94822 L 365.66298,520.71139 L 372.32386,520.9073 L 373.51612,522.21432 L 374.70346,522.07579 L 375.26249,520.80934 L 385.71982,520.2167 L 389.80971,517.39343 L 394.31518,512.97709 L 397.13207,509.10034 L 402.94499,502.34473 L 409.20709,497.34584 L 412.92815,491.4214 z"},
    // Light 1777: The Storm Peaks (Ulduar sky)
    {571, 1777, "L 497.52381,385.87012 L 497.31602,433.38528 L 500.50217,433.87013 L 501.19481,444.05194 L 506.59741,453.74892 L 506.73594,460.53679 L 513.73927,465.6626 L 516.54638,468.89212 L 516.96245,471.54364 L 519.49106,474.95708 L 519.52617,479.54801 L 519.29757,485.57887 L 522.68513,487.44333 L 540.71684,487.39769 L 543.89051,486.15093 L 546.10288,484.75605 L 549.63908,484.82328 L 549.81818,479.86147 L 559.65368,479.44589 L 559.65368,475.98268 L 563.04762,475.08225 L 570.25108,474.52813 L 570.94373,468.64069 L 577.38528,468.64069 L 576.69264,463.65368 L 581.95671,460.74459 L 584.72727,462.68398 L 592.90043,462.54545 L 593.87013,458.66667 L 634.45888,458.66667 L 634.8052,443.84415 L 641.59308,443.63636 L 642.00866,386.07792 L 593.67422,354.19269 L 497.42571,353.74348 z"},
    // Light 1792: Wintergrasp (smoky sky)
    {571, 1792, "L 434.6833,493.4155 L 431.5591,489.5666 L 421.74892,489.6277 L 421.61039,491.5671 L 412.81385,491.42857 L 409.0736,497.24675 L 402.90909,502.37229 L 397.02164,509.2987 L 394.11255,513.17749 L 389.74892,517.4026 L 385.76039,520.14744 L 385.84527,526.88454 L 387.80724,529.60553 L 401.6142,530.50597 L 415.65119,531.25887 L 417.99501,534.96372 L 420.71323,535.60043 L 426.6942,536.20207 L 435.4797,536.57301 L 435.99408,534.62088 L 440.10816,534.42498 L 444.71201,529.82113 L 448.92404,529.82113 L 452.25448,530.89863 L 452.6463,532.26998 L 459.699,531.48635 L 461.75603,529.42931 L 461.65808,521.39707 L 459.699,519.34003 L 459.97606,512.71769 L 460.02529,510.62296 L 456.67887,510.60734 L 453.57677,506.7564 L 451.0751,505.99281 L 449.65176,501.16809 L 446.77137,496.50482 L 440.60266,494.0864 z"},
    // Light 1589: Zul'Drak (ZulDrakSkyA)
    {571, 1589, "L 642.15663,386.15103 L 641.52833,443.72348 L 634.72906,443.72601 L 634.52127,458.70639 L 593.70032,458.70639 L 593.01464,462.72251 L 584.59058,462.72251 L 582.06058,460.90196 L 576.75425,463.70205 L 577.4182,468.73745 L 570.85816,468.70876 L 570.34208,474.64423 L 562.87752,475.15777 L 559.71022,475.84837 L 559.61227,479.57063 L 549.71889,479.86449 L 549.64963,484.92246 L 550.60048,486.42743 L 550.60048,492.01081 L 549.71889,492.79445 L 549.71889,495.83103 L 546.68231,499.16147 L 546.58436,506.70394 L 555.49819,506.70394 L 556.86955,511.89551 L 553.82109,515.68704 L 554.75778,517.92127 L 557.82502,517.04701 L 562.09501,514.5126 L 563.04066,516.49936 L 569.01587,517.33443 L 584.91316,517.84232 L 585.012,520.48302 L 588.11694,521.49503 L 589.09648,523.94388 L 595.16964,524.53161 L 600.85099,524.43364 L 609.373,523.94388 L 610.05867,521.9848 L 618.18887,516.89118 L 625.63338,512.38529 L 626.22112,509.54461 L 628.66996,508.07531 L 638.56334,507.19371 L 641.99174,503.56941 L 646.59558,503.47145 L 650.80761,500.53283 L 651.10148,493.87194 L 652.66874,491.81491 L 658.53021,491.88417 L 695.82532,480.31081 L 677.03249,424.56931 z"},
    // Light 1740: Crystalsong Forest (no skybox)
    {571, 1740, "L 519.29515,485.58511 L 519.52617,479.54801 L 509.69114,479.18656 L 505.57186,478.7797 L 503.42658,474.56332 L 499.65934,474.56359 L 496.37696,470.65044 L 489.78598,470.68855 L 489.70965,467.82033 L 486.61957,468.09573 L 479.68237,472.87522 L 474.97836,473.80086 L 473.07359,477.78355 L 470.92641,480.65801 L 471.1342,486.64935 L 473.37993,490.21746 L 473.4716,490.31815 L 475.42857,492.46753 L 477.67966,493.4026 L 481.41992,494.96104 L 481.52381,496.83116 L 485.81818,503.54978 L 490.8052,505.35065 L 497.31602,505.35065 L 499.80953,501.47186 L 511.30736,502.02597 L 520.17316,507.01299 L 520.17316,507.01299 L 526.68398,512.13853 L 542.19914,512.55411 L 546.63204,506.5974 L 546.77056,499.25541 L 549.67965,495.65368 L 549.67965,492.88312 L 550.64935,491.63636 L 550.51082,486.37229 L 549.59011,484.92123 L 545.80206,484.85713 L 543.63817,486.25371 L 540.65057,487.4923 L 522.69277,487.41222 z"},
  };

  // FUN_0077ed40 (startup): SVG units -> world: x' = (x + OX) * 33.333, z' = ((y + OY) * 2^-10) * 34133.33.
  // x' pairs with 17066.67 - client_y (= noggit x), z' with 17066.67 - client_x (= noggit z). The box is grown
  // by the 50-yard fade (DAT_009f22ec) so the polygon test only runs where it can matter.
  constexpr float ZONE_POLY_OFFSET_X = -1.6623375415802002f; // DAT_00a3e8ac
  constexpr float ZONE_POLY_OFFSET_Y = -145.73159790039062f; // DAT_00a3e8a8
  constexpr float ZONE_POLY_SCALE_X = 33.33333206176758f;    // DAT_00a3e554
  constexpr float ZONE_POLY_SCALE_Y1 = 0.0009765625f;        // DAT_00a1c8a0
  constexpr float ZONE_POLY_SCALE_Y2 = 34133.33203125f;      // DAT_009e2ac8
  constexpr float ZONE_POLY_FADE = 50.f;                     // DAT_009f22ec
  constexpr std::size_t ZONE_POLY_MAX_ACTIVE = 5;            // FUN_007ed150: the list holds five

  std::vector<glm::vec2> parse_zone_polygon(char const* svg)
  {
    std::vector<glm::vec2> pts;
    for (char const* p = svg; *p && *p != 'z'; ++p)
    {
      if (*p != 'M' && *p != 'L')
      {
        continue;
      }
      char const* xs = p + 2; // "L x,y "
      char const* comma = xs;
      while (*comma && *comma != ',')
      {
        ++comma;
      }
      if (!*comma)
      {
        break;
      }
      char const* ys = comma + 1;
      float const x = static_cast<float>(std::atof(xs));
      float const y = static_cast<float>(std::atof(ys));
      pts.emplace_back((x + ZONE_POLY_OFFSET_X) * ZONE_POLY_SCALE_X
                      , ((y + ZONE_POLY_OFFSET_Y) * ZONE_POLY_SCALE_Y1) * ZONE_POLY_SCALE_Y2);
      p = ys;
      while (*p && *p != ' ')
      {
        ++p;
      }
      if (!*p)
      {
        break;
      }
    }
    return pts;
  }

  // FUN_007f9c90: even-odd crossing test plus the distance to the nearest edge (FUN_007f9bf0 = squared
  // point-to-segment distance); the edge between the last and the first point closes the polygon. Negative
  // inside.
  float zone_polygon_signed_distance(std::vector<glm::vec2> const& pts, float x, float z)
  {
    float best = std::numeric_limits<float>::max();
    bool inside = false;
    std::size_t const n = pts.size();
    for (std::size_t i = 0; i < n; ++i)
    {
      glm::vec2 const& a = pts[i];
      glm::vec2 const& b = pts[(i + n - 1) % n];
      float const dx = b.x - a.x;
      float const dz = b.y - a.y;
      float const len2 = dx * dx + dz * dz;
      float d2;
      if (len2 > 0.f)
      {
        float const t = ((x - a.x) * dx + (z - a.y) * dz) / len2;
        if (t < 0.f)
        {
          d2 = (x - a.x) * (x - a.x) + (z - a.y) * (z - a.y);
        }
        else if (t > 1.f)
        {
          d2 = (x - b.x) * (x - b.x) + (z - b.y) * (z - b.y);
        }
        else
        {
          float const px = x - (a.x + dx * t);
          float const pz = z - (a.y + dz * t);
          d2 = px * px + pz * pz;
        }
      }
      else
      {
        d2 = (x - a.x) * (x - a.x) + (z - a.y) * (z - a.y);
      }
      best = std::min(best, d2);
      // client: (a.y < z && z <= b.y) || (b.y < z && z <= a.y), then the crossing's x against the point
      if ((a.y < z && z <= b.y) || (b.y < z && z <= a.y))
      {
        float const xi = (b.x - a.x) * ((z - a.y) / (b.y - a.y)) + a.x;
        if (xi < x)
        {
          inside = !inside;
        }
      }
    }
    float const dist = std::sqrt(best);
    return inside ? -dist : dist;
  }

  // FUN_0077eed0 queues value = 50 - signed distance when that is positive; FUN_007ee6b0 turns it into
  // weight = clamp(value / 100, 0, 1): full 50 yards inside the edge, half on it, gone 50 yards outside.
  float zone_polygon_weight(float signed_distance)
  {
    if (!(signed_distance - ZONE_POLY_FADE < 0.f))
    {
      return 0.f;
    }
    float const value = -(signed_distance - ZONE_POLY_FADE);
    float const f = (100.f - value) * 0.01f;
    float const w = (f <= 0.f) ? 1.f : 1.f - f;
    return std::max(0.f, std::min(1.f, w));
  }
  struct RawDBC
  {
    bool valid = false;
    std::uint32_t record_count = 0;
    std::uint32_t field_count = 0;
    std::uint32_t record_size = 0;
    std::uint32_t string_size = 0;
    std::vector<std::uint32_t> records;
    std::vector<char> strings;

    std::uint32_t word(std::size_t row, std::size_t field) const
    {
      return records[row * field_count + field];
    }

    float number(std::size_t row, std::size_t field) const
    {
      float value = 0.f;
      std::uint32_t raw = word(row, field);
      std::memcpy(&value, &raw, sizeof(value));
      return value;
    }

    const char* string(std::size_t row, std::size_t field) const
    {
      std::uint32_t offset = word(row, field);
      return offset < strings.size() ? strings.data() + offset : "";
    }
  };

  RawDBC load_raw_dbc(char const* filename)
  {
    RawDBC dbc;
    BlizzardArchive::ClientFile file(filename, Noggit::Application::NoggitApplication::instance()->clientData());
    if (file.isEof() || file.getSize() < 20)
    {
      return dbc;
    }

    char magic[4] = {};
    file.read(magic, 4);
    if (std::memcmp(magic, "WDBC", 4) != 0)
    {
      return dbc;
    }

    file.read(&dbc.record_count, 4);
    file.read(&dbc.field_count, 4);
    file.read(&dbc.record_size, 4);
    file.read(&dbc.string_size, 4);
    if (dbc.field_count == 0 || dbc.record_size != dbc.field_count * 4)
    {
      return dbc;
    }

    dbc.records.resize(static_cast<std::size_t>(dbc.record_count) * dbc.field_count);
    file.read(dbc.records.data(), dbc.records.size() * sizeof(std::uint32_t));
    dbc.strings.resize(dbc.string_size);
    file.read(dbc.strings.data(), dbc.strings.size());
    dbc.valid = true;
    return dbc;
  }

  int find_raw_row_by_id(RawDBC const& dbc, std::uint32_t id)
  {
    if (!dbc.valid || dbc.field_count == 0)
    {
      return -1;
    }

    for (std::size_t row = 0; row < dbc.record_count; ++row)
    {
      if (dbc.word(row, 0) == id)
      {
        return static_cast<int>(row);
      }
    }

    return -1;
  }

  void fill_raw_sky_color_bands(SkyParam* param, std::uint32_t param_id, RawDBC const& light_int_band)
  {
    int light_int_start = static_cast<int>(param_id) * NUM_SkyColorNames - 17;

    for (int color_index = 0; color_index < NUM_SkyColorNames; ++color_index)
    {
      int row = find_raw_row_by_id(light_int_band, static_cast<std::uint32_t>(light_int_start + color_index));
      if (row < 0 || light_int_band.field_count <= LightIntBandDB::Values)
      {
        param->mmin[color_index] = -1;
        continue;
      }

      std::uint32_t entries = std::min<std::uint32_t>(light_int_band.word(row, LightIntBandDB::Entries), 16);
      if (entries == 0)
      {
        param->mmin[color_index] = -1;
        continue;
      }

      param->mmin[color_index] = static_cast<int>(light_int_band.word(row, LightIntBandDB::Times));
      for (std::uint32_t entry = 0; entry < entries; ++entry)
      {
        param->colorRows[color_index].emplace_back(static_cast<int>(light_int_band.word(row, LightIntBandDB::Times + entry))
                                                  , static_cast<int>(light_int_band.word(row, LightIntBandDB::Values + entry)));
      }
    }
  }

  void fill_raw_sky_float_bands(SkyParam* param, std::uint32_t param_id, RawDBC const& light_float_band)
  {
    int light_float_start = static_cast<int>(param_id) * NUM_SkyFloatParamsNames - 5;

    for (int float_index = 0; float_index < NUM_SkyFloatParamsNames; ++float_index)
    {
      int row = find_raw_row_by_id(light_float_band, static_cast<std::uint32_t>(light_float_start + float_index));
      if (row < 0 || light_float_band.field_count <= LightFloatBandDB::Values)
      {
        param->mmin_float[float_index] = -1;
        continue;
      }

      std::uint32_t entries = std::min<std::uint32_t>(light_float_band.word(row, LightFloatBandDB::Entries), 16);
      if (entries == 0)
      {
        param->mmin_float[float_index] = -1;
        continue;
      }

      param->mmin_float[float_index] = static_cast<int>(light_float_band.word(row, LightFloatBandDB::Times));
      for (std::uint32_t entry = 0; entry < entries; ++entry)
      {
        param->floatParams[float_index].emplace_back(static_cast<int>(light_float_band.word(row, LightFloatBandDB::Times + entry))
                                                    , light_float_band.number(row, LightFloatBandDB::Values + entry));
      }
    }
  }

  SkyParam* make_raw_sky_param(std::uint32_t param_id, RawDBC const& light_params, RawDBC const& light_skybox, RawDBC const& light_int_band, RawDBC const& light_float_band, Noggit::NoggitRenderContext context)
  {
    auto* param = new SkyParam(0, context);
    param->Id = static_cast<int>(param_id);
    fill_raw_sky_color_bands(param, param_id, light_int_band);
    fill_raw_sky_float_bands(param, param_id, light_float_band);

    int row = find_raw_row_by_id(light_params, param_id);
    if (row < 0)
    {
      return param;
    }

    param->set_highlight_sky(light_params.field_count > 1 && light_params.word(row, 1) != 0);

    // The classic 9-field LightParams.dbc is the SAME layout as modern for fields 0-8 (verified vs
    // the Turtle DBC: f5/6 = water shallow/deep, f7/8 = ocean shallow/deep; only the trailing
    // `flags` field 9 is absent). The old classic branch was OFF BY ONE on the float fields ->
    // ocean_shallow read waterDeepAlpha (opaque shore) AND glow read field 3 (cloudTypeID, always 0),
    // which was then "rescued" by substituting LightFloatBand band 3 -- but band 3 is CLOUD DENSITY
    // (the cloud-coverage input, wow.exe FUN_006d0970), NOT glow. glow is field 4 and is NOT zeroed:
    // LightParams.dbc field 4 (float) = 0.6 for Karazhan 386 (== the measured 0.600 FFXGlow composite,
    // wow_cap_kara), 0.65 for the map-0 default day 12, 0.0 for Dun Morogh's Steelgrill's 406. Reading
    // cloud density (0.95 at overcast Steelgrill's) as the FFXGlow strength was the fog over-bloom bug
    // -- use field 4, floored to 0.329 outdoors in the render (client FFXEffects.cpp FUN_006cb020).
    std::size_t const glow_field = LightParamsDB::glow;
    std::size_t const river_shallow_field = LightParamsDB::water_shallow_alpha;
    std::size_t const river_deep_field = LightParamsDB::water_deep_alpha;
    std::size_t const ocean_shallow_field = LightParamsDB::ocean_shallow_alpha;
    std::size_t const ocean_deep_field = LightParamsDB::ocean_deep_alpha;

    if (light_params.field_count > glow_field)
      param->set_glow(light_params.number(row, glow_field));
    if (light_params.field_count > river_shallow_field)
      param->set_river_shallow_alpha(light_params.number(row, river_shallow_field));
    if (light_params.field_count > river_deep_field)
      param->set_river_deep_alpha(light_params.number(row, river_deep_field));
    if (light_params.field_count > ocean_shallow_field)
      param->set_ocean_shallow_alpha(light_params.number(row, ocean_shallow_field));
    if (light_params.field_count > ocean_deep_field)
      param->set_ocean_deep_alpha(light_params.number(row, ocean_deep_field));

    if (light_params.field_count > 2)
    {
      int skybox_row = find_raw_row_by_id(light_skybox, light_params.word(row, 2));
      if (skybox_row >= 0 && light_skybox.field_count > 1)
      {
        const char* filename = light_skybox.string(skybox_row, 1);
        if (filename && *filename)
        {
          param->skybox.emplace(filename, context);
          param->skybox_id = static_cast<int>(light_params.word(row, 2));
          // Field 2 = flags (WotLK-only column; absent on 1.12 -> field_count 2, so stays 0).
          if (light_skybox.field_count > 2)
          {
            param->skybox_flags = light_skybox.word(skybox_row, 2);
          }
        }
      }
    }

    return param;
  }

  glm::vec3 default_sky_color(int row)
  {
    switch (row)
    {
      case LIGHT_GLOBAL_DIFFUSE:
        return {0.82f, 0.78f, 0.70f};
      case LIGHT_GLOBAL_AMBIENT:
        return {0.45f, 0.50f, 0.55f};
      case SKY_COLOR_0:
        return {0.22f, 0.40f, 0.72f};
      case SKY_COLOR_1:
        return {0.35f, 0.55f, 0.82f};
      case SKY_COLOR_2:
      case SKY_COLOR_3:
        return {0.54f, 0.68f, 0.88f};
      case SKY_COLOR_4:
      case FOG_COLOR:
        return {0.70f, 0.78f, 0.86f};
      case SHADOW_OPACITY:
        return {0.35f, 0.35f, 0.35f};
      case SUN_COLOR:
      case SUN_HALO_COLOR:
        return {1.0f, 0.90f, 0.72f};
      case CLOUD_EDGE_COLOR:
      case CLOUD_COLOR:
        return {0.80f, 0.82f, 0.85f};
      case OCEAN_COLOR_LIGHT:
      case RIVER_COLOR_LIGHT:
        return {0.22f, 0.45f, 0.55f};
      case OCEAN_COLOR_DARK:
      case RIVER_COLOR_DARK:
        return {0.04f, 0.18f, 0.28f};
      default:
        return {0.65f, 0.65f, 0.65f};
    }
  }

  bool drawable_model_instance(ModelInstance& model)
  {
    return model.model->finishedLoading() && !model.model->loading_failed();
  }

  SkyParam* active_sky_param(Sky& sky)
  {
    if (sky.curr_sky_param < 0 || sky.curr_sky_param >= NUM_SkyParamsNames)
    {
      return nullptr;
    }

    return sky.skyParams[sky.curr_sky_param];
  }

  SkyParam const* active_sky_param(Sky const& sky)
  {
    if (sky.curr_sky_param < 0 || sky.curr_sky_param >= NUM_SkyParamsNames)
    {
      return nullptr;
    }

    return sky.skyParams[sky.curr_sky_param];
  }

  SkyParam* drawable_skybox_param(Sky& sky)
  {
    SkyParam* current = active_sky_param(sky);
    if (current && current->skybox && drawable_model_instance(current->skybox.value()))
    {
      return current;
    }

    return nullptr;
  }

  float default_sky_float_param(int row)
  {
    switch (row)
    {
      case FOG_DISTANCE:
        return 18000.0f;
      case FOG_MULTIPLIER:
        return 0.25f;
      default:
        return 0.0f;
    }
  }
}

SkyColor::SkyColor(int t, int col)
{
  time = t;
  color.z = ((col & 0x0000ff)) / 255.0f;
  color.y = ((col & 0x00ff00) >> 8) / 255.0f;
  color.x = ((col & 0xff0000) >> 16) / 255.0f;
}

SkyFloatParam::SkyFloatParam(int t, float val)
: time(t)
, value(val)
{
}

SkyParam::SkyParam(int paramId, Noggit::NoggitRenderContext context)
: _context(context)
{
    Id = paramId;

    for (int i = 0; i < 36; ++i)
    {
        mmin[i] = -2;
    }

    for (int i = 0; i < 6; ++i)
    {
        mmin_float[i] = -2;
    }

    if (paramId == 0)
      return; // don't initialise entry

    // int light_param_0 = data->getInt(LightDB::DataIDs);
    int light_int_start = paramId * NUM_SkyColorNames - 17;

    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
        try
        {
            DBCFile::Record rec = gLightIntBandDB.getByID(light_int_start + i);
            int entries = rec.getInt(LightIntBandDB::Entries);

            if (entries == 0)
            {
                mmin[i] = -1;
            }
            else
            {
                mmin[i] = rec.getInt(LightIntBandDB::Times);
                for (int l = 0; l < entries; l++)
                {
                    SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
                    colorRows[i].push_back(sc);
                }
            }
        }
        catch (...)
        {
            // LogError << "When trying to intialize sky " << data->getInt(LightDB::ID) << ", there was an error with getting an entry in a DBC (" << i << "). Sorry." << std::endl;
            LogError << "When trying to intialize sky, there was an error with getting an entry in LightIntBand DBC (" << i << "). Sorry." << std::endl;
            // Fallback = the SAME band of LightParams 1 (ids 1..18), not band id `i`: id 0 never exists, so the
            // old `getByID(i)` threw INSIDE this handler and std::terminate() took the process down -- the
            // Forever Beta ships 827 LightParams rows but LightData for 815, and a map whose light names one
            // of the 12 empty params (Caverns of Time, Development Land) crashed on open (crash dump
            // noggit.exe.21860: DBCFile::NotFound out of SkyParam::SkyParam catch$12). WotLK data has no
            // gaps, so this path never ran there. docs/client_re/42 sec 29.
            mmin[i] = -1;
            colorRows[i].clear();
            int const fallback_id = 1 + i;
            if (gLightIntBandDB.CheckIfIdExists(fallback_id))
            {
                try
                {
                    DBCFile::Record rec = gLightIntBandDB.getByID(fallback_id);
                    int entries = rec.getInt(LightIntBandDB::Entries);
                    if (entries > 0)
                    {
                        mmin[i] = rec.getInt(LightIntBandDB::Times);
                        for (int l = 0; l < entries; l++)
                        {
                            SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
                            colorRows[i].push_back(sc);
                        }
                    }
                }
                catch (...)
                {
                    mmin[i] = -1;
                    colorRows[i].clear();
                }
            }
        }
    }

    int light_float_start = paramId * NUM_SkyFloatParamsNames - 5;

    for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
    {
        try
        {
            DBCFile::Record rec = gLightFloatBandDB.getByID(light_float_start + i);
            int entries = rec.getInt(LightFloatBandDB::Entries);

            if (entries == 0)
            {
                mmin_float[i] = -1;
            }
            else
            {
                mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
                for (int l = 0; l < entries; l++)
                {
                    SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
                    floatParams[i].push_back(sc);
                }
            }
        }
        catch (...)
        {
            LogError << "When trying to intialize sky, there was an error with getting an entry in LightFloatBand DBC (" << i << "). Sorry." << std::endl;
            // Same rule as the colour bands above: param 1's band (ids 1..6), guarded, never a throw from here.
            mmin_float[i] = -1;
            floatParams[i].clear();
            int const fallback_id = 1 + i;
            if (gLightFloatBandDB.CheckIfIdExists(fallback_id))
            {
                try
                {
                    DBCFile::Record rec = gLightFloatBandDB.getByID(fallback_id);
                    int entries = rec.getInt(LightFloatBandDB::Entries);
                    if (entries > 0)
                    {
                        mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
                        for (int l = 0; l < entries; l++)
                        {
                            SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
                            floatParams[i].push_back(sc);
                        }
                    }
                }
                catch (...)
                {
                    mmin_float[i] = -1;
                    floatParams[i].clear();
                }
            }
        }
    }

    try
    {
        DBCFile::Record light_param = gLightParamsDB.getByID(paramId);
        int skybox_id = light_param.getInt(LightParamsDB::skybox);
        // See the raw-DBC path above: the classic 9-field layout matches modern for fields 0-8, so the
        // old off-by-one made ocean_shallow read waterDeepAlpha AND glow read field 3 (cloudTypeID) then
        // substitute cloud-density band 3 (the over-bloom bug). Use real columns -- glow is field 4.
        std::size_t const glow_field = LightParamsDB::glow;
        std::size_t const river_shallow_field = LightParamsDB::water_shallow_alpha;
        std::size_t const river_deep_field = LightParamsDB::water_deep_alpha;
        std::size_t const ocean_shallow_field = LightParamsDB::ocean_shallow_alpha;
        std::size_t const ocean_deep_field = LightParamsDB::ocean_deep_alpha;

        _highlight_sky = light_param.getInt(LightParamsDB::highlightSky);
        _glow = light_param.getFloat(glow_field);
        _river_shallow_alpha = light_param.getFloat(river_shallow_field);
        _river_deep_alpha = light_param.getFloat(river_deep_field);
        _ocean_shallow_alpha = light_param.getFloat(ocean_shallow_field);
        _ocean_deep_alpha = light_param.getFloat(ocean_deep_field);

        // [2026-09-01] A LightParams row that carries ZERO in the water-alpha columns is not
        // authoring "invisible water" -- it means those columns were never filled, which is normal
        // for custom/ported zones. The shader computes opacity as mix(shallow_a, deep_a, depth),
        // so shallow_a = 0 makes every physically-shallow liquid cell fully transparent while the
        // deeper cells stay solid: the water breaks up into a depth-following checkerboard of
        // holes. Keep the sane defaults when the row is unauthored (BOTH ends zero -- a genuine
        // 0 -> 1 ramp is a legitimate authored shore fade and is left alone).
        if (_river_shallow_alpha <= 0.0f && _river_deep_alpha <= 0.0f)
        {
          _river_shallow_alpha = 0.6f;
          _river_deep_alpha = 1.0f;
        }
        if (_ocean_shallow_alpha <= 0.0f && _ocean_deep_alpha <= 0.0f)
        {
          _ocean_shallow_alpha = 0.75f;
          _ocean_deep_alpha = 1.0f;
        }
        {
          static int logged_water_alpha = 0;
          if (logged_water_alpha < 12)
          {
            LogError << "[WATER-ALPHA] lightParam=" << paramId
                     << " riverShallow=" << _river_shallow_alpha
                     << " riverDeep=" << _river_deep_alpha
                     << " oceanShallow=" << _ocean_shallow_alpha
                     << " oceanDeep=" << _ocean_deep_alpha << std::endl;
            logged_water_alpha++;
          }
        }

        if (skybox_id)
        {
            DBCFile::Record const skybox_rec = gLightSkyboxDB.getByID(skybox_id);
            // modern clients name the skybox model by fileDataID (synthesized column 3); the string
            // column then carries the listfile path (or an "fdid:" pseudo name) for display
            std::uint32_t const skybox_fdid = gLightSkyboxDB.getFieldCount() > LightSkyboxDB::skyboxFileDataID
                                            ? skybox_rec.getUInt(LightSkyboxDB::skyboxFileDataID) : 0u;
            std::string const skybox_name = skybox_rec.getString(LightSkyboxDB::filename);
            if (skybox_fdid && skybox_name.rfind("fdid:", 0) == 0)
              skybox.emplace(BlizzardArchive::Listfile::FileKey(skybox_fdid), _context);
            else if (skybox_fdid)
              skybox.emplace(BlizzardArchive::Listfile::FileKey(skybox_name, skybox_fdid), _context);
            else
              skybox.emplace(skybox_name, _context);
            // [2026-09-09] the id and flags were only stored by the raw-DBC fallback loader: on this path
            // every param read skybox_id 0 / flags 0, so Skies::draw merged EVERY skybox into one slot
            // (the citadel vanished under the zone light) and never applied the flag-1 day mapping or
            // the flag-2 overlay rule.
            this->skybox_id = skybox_id;
            // 1.12 LightSkybox.dbc has no flags column (ID and file name only)
            skybox_flags = gLightSkyboxDB.getFieldCount() > LightSkyboxDB::flags ? skybox_rec.getInt(LightSkyboxDB::flags) : 0;
        }
    }
    catch (...)
    {
        LogError << "When trying to get the skybox for the entry " << paramId << " in LightParams.dbc. Sad." << std::endl;
    }
}


Sky::Sky(DBCFile::Iterator data, Noggit::NoggitRenderContext context)
: _context(context)
, _selected(false)
{
  Id = data->getInt(LightDB::ID);
  pos = glm::vec3(data->getFloat(LightDB::PositionX) / skymul, data->getFloat(LightDB::PositionY) / skymul, data->getFloat(LightDB::PositionZ) / skymul);
  r1 = data->getFloat(LightDB::RadiusInner) / skymul;
  r2 = data->getFloat(LightDB::RadiusOuter) / skymul;

  // for (int i = 0; i < 36; ++i)
  // {
  //   mmin[i] = -2;
  // }

  // for (int i = 0; i < 6; ++i)
  // {
  //   mmin_float[i] = -2;
  // }

  global = (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f);

  // int light_param_0 = data->getInt(LightDB::DataIDs);
  // int light_int_start = light_param_0 * NUM_SkyColorNames - 17;

    for (int i = 0; i < NUM_SkyParamsNames; ++i)
    {
      skyParams[i] = nullptr;
    }

    size_t const available_sky_params = gLightDB.getFieldCount() > LightDB::DataIDs
                      ? std::min<size_t>(NUM_SkyParamsNames, gLightDB.getFieldCount() - LightDB::DataIDs)
                      : 0;

    for (size_t i = 0; i < available_sky_params; ++i)
  {
      int sky_param_id = data->getInt(LightDB::DataIDs + i);
      if (sky_param_id == 0)
      {
          skyParams[i] = nullptr;
          continue;
      }

      SkyParam* sky_param = new SkyParam(sky_param_id, _context);
        skyParams[i] = sky_param;
  }

  // for (int i = 0; i < NUM_SkyColorNames; ++i)
  // {
  //   try
  //   {
  //     DBCFile::Record rec = gLightIntBandDB.getByID(light_int_start + i);
  //     int entries = rec.getInt(LightIntBandDB::Entries);
  // 
  //     if (entries == 0)
  //     {
  //       mmin[i] = -1;
  //     }
  //     else
  //     {
  //       mmin[i] = rec.getInt(LightIntBandDB::Times);
  //       for (int l = 0; l < entries; l++)
  //       {
  //         SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
  //         colorRows[i].push_back(sc);
  //       }
  //     }
  //   }
  //   catch (...)
  //   {
  //     LogError << "When trying to intialize sky " << data->getInt(LightDB::ID) << ", there was an error with getting an entry in a DBC (" << i << "). Sorry." << std::endl;
  //     DBCFile::Record rec = gLightIntBandDB.getByID(i);
  //     int entries = rec.getInt(LightIntBandDB::Entries);
  // 
  //     if (entries == 0)
  //     {
  //       mmin[i] = -1;
  //     }
  //     else
  //     {
  //       mmin[i] = rec.getInt(LightIntBandDB::Times);
  //       for (int l = 0; l < entries; l++)
  //       {
  //         SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
  //         colorRows[i].push_back(sc);
  //       }
  //     }
  //   }
  // }
  // 
  // int light_float_start = light_param_0 * NUM_SkyFloatParamsNames - 5;
  // 
  // for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
  // {
  //   try
  //   {
  //     DBCFile::Record rec = gLightFloatBandDB.getByID(light_float_start + i);
  //     int entries = rec.getInt(LightFloatBandDB::Entries);
  // 
  //     if (entries == 0)
  //     {
  //       mmin_float[i] = -1;
  //     }
  //     else
  //     {
  //       mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
  //       for (int l = 0; l < entries; l++)
  //       {
  //         SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
  //         floatParams[i].push_back(sc);
  //       }
  //     }
  //   }
  //   catch (...)
  //   {
  //     LogError << "When trying to intialize sky " << data->getInt(LightDB::ID) << ", there was an error with getting an entry in a DBC (" << i << "). Sorry." << std::endl;
  //     DBCFile::Record rec = gLightFloatBandDB.getByID(i);
  //     int entries = rec.getInt(LightFloatBandDB::Entries);
  // 
  //     if (entries == 0)
  //     {
  //       mmin_float[i] = -1;
  //     }
  //     else
  //     {
  //       mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
  //       for (int l = 0; l < entries; l++)
  //       {
  //         SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
  //         floatParams[i].push_back(sc);
  //       }
  //     }
  //   }
  // }
  // 
  // try
  // {
  //   DBCFile::Record light_param = gLightParamsDB.getByID(light_param_0);
  //   int skybox_id = light_param.getInt(LightParamsDB::skybox);
  // 
  //   _highlight_sky = light_param.getInt(LightParamsDB::highlightSky);
  //   _river_shallow_alpha = light_param.getFloat(LightParamsDB::water_shallow_alpha);
  //   _river_deep_alpha = light_param.getFloat(LightParamsDB::water_deep_alpha);
  //   _ocean_shallow_alpha = light_param.getFloat(LightParamsDB::ocean_shallow_alpha);
  //   _ocean_deep_alpha = light_param.getFloat(LightParamsDB::ocean_deep_alpha);
  //   _glow = light_param.getFloat(LightParamsDB::glow);
  // 
  //   if (skybox_id)
  //   {
  //     skybox.emplace(gLightSkyboxDB.getByID(skybox_id).getString(LightSkyboxDB::filename), _context);
  //   }
  // }
  // catch (...)
  // {
  //   LogError << "When trying to get the skybox for the entry " << light_param_0 << " in LightParams.dbc. Sad." << std::endl;
  // }
}

Sky::Sky(int id, glm::vec3 const& position, float inner_radius, float outer_radius, std::vector<SkyParam*> params, Noggit::NoggitRenderContext context)
: _context(context)
, _selected(false)
{
  Id = id;
  pos = position;
  r1 = inner_radius;
  r2 = outer_radius;
  global = (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f);
  weight = 0.f;
  is_new_record = false;
  std::memset(name, 0, sizeof(name));

  for (int i = 0; i < NUM_SkyParamsNames; ++i)
  {
    skyParams[i] = i < params.size() ? params[i] : nullptr;
  }
}

namespace
{
  // Editor weather intensity (0 = clear .. 1 = full storm), set per-frame by the renderer from the
  // weather control. File-scope: Sky::colorFor/floatParamFor are per-sky const calls with no Skies
  // backpointer.
  float g_weather_intensity = 0.0f;
}

void Skies::set_weather_intensity(float w)
{
  g_weather_intensity = std::clamp(w, 0.0f, 1.0f);
}

float Skies::weather_intensity()
{
  return g_weather_intensity;
}

namespace
{
  // The WATER param's river-deep band, weighted across the active lights (computed each frame in
  // update_sky_colors) -- the flat colour for exterior WMO liquid (canal dark blue). File-scope
  // static for the same reason as the weather intensity.
  glm::vec3 g_water_river_dark(0.05f, 0.15f, 0.25f);
}

void Skies::set_water_river_dark(glm::vec3 const& c)
{
  g_water_river_dark = c;
}

glm::vec3 Skies::water_river_dark()
{
  return g_water_river_dark;
}

float Sky::floatParamForAirVariant(int r, int t) const
{
  // The AIR twin of the current param: submersion swaps CLEAR->CLEAR_WATER (and storm blending
  // uses STORM_WATER), but some bands must not follow -- clouds exist above the surface no matter
  // where the camera is (user 2026-08-26: the underwater param's empty CLOUD_DENSITY zeroed the
  // coverage; surfacing re-grew the cloud texture rows-per-frame = the "helmet closing" banding).
  int const air_idx = (curr_sky_param == CLEAR_WATER) ? CLEAR
                    : (curr_sky_param == STORM_WATER) ? STORM
                    : curr_sky_param;
  SkyParam const* p = skyParams[air_idx];
  float const base = p ? floatFromParam(p, r, t) : floatParamFor(r, t);
  if (p && g_weather_intensity > 0.0f)
  {
    SkyParam const* storm = skyParams[STORM];
    if (storm)
    {
      return base + (floatFromParam(storm, r, t) - base) * g_weather_intensity;
    }
  }
  return base;
}

float Sky::floatParamFor(int r, int t) const
{
  float const base = floatFromParam(active_sky_param(*this), r, t);
  if (g_weather_intensity > 0.0f)
  {
    // The client's rainy weather runs the zone light's STORM param set; blend by intensity.
    int const storm_idx = (curr_sky_param == CLEAR_WATER) ? STORM_WATER : STORM;
    SkyParam const* storm = skyParams[storm_idx];
    if (storm)
    {
      return base + (floatFromParam(storm, r, t) - base) * g_weather_intensity;
    }
  }
  return base;
}

float Sky::floatFromParam(SkyParam const* sky_param, int r, int t) const
{
  if (!sky_param || r < 0 || r >= NUM_SkyFloatParamsNames || sky_param->mmin_float[r] < 0 || sky_param->floatParams[r].empty())
  {
    return default_sky_float_param(r);
  }
  float c1, c2;
  int t1, t2;
  size_t last = sky_param->floatParams[r].size() - 1;

  if (t< sky_param->mmin_float[r])
  {
    // reverse interpolate
    c1 = sky_param->floatParams[r][last].value;
    c2 = sky_param->floatParams[r][0].value;
    t1 = sky_param->floatParams[r][last].time;
    t2 = sky_param->floatParams[r][0].time + 2880;
    t += 2880;
  }
  else
  {
    for (size_t i = last; true; i--)
    { //! \todo iterator this.
      if (sky_param->floatParams[r][i].time <= t)
      {
        c1 = sky_param->floatParams[r][i].value;
        t1 = sky_param->floatParams[r][i].time;

        if (i == last)
        {
          c2 = sky_param->floatParams[r][0].value;
          t2 = sky_param->floatParams[r][0].time + 2880;
        }
        else
        {
          c2 = sky_param->floatParams[r][i + 1].value;
          t2 = sky_param->floatParams[r][i + 1].time;
        }
        break;
      }
    }
  }

  float tt = static_cast<float>(t - t1) / static_cast<float>(t2 - t1);
  return c1 + ((c2 - c1) * tt);
}

glm::vec3 Sky::colorFor(int r, int t) const
{
  glm::vec3 const base = colorFromParam(active_sky_param(*this), r, t);
  if (g_weather_intensity > 0.0f)
  {
    // Rainy weather: blend every band toward the zone light's STORM param set (client mechanism —
    // e.g. WPL storm param 473 turns the orange clear fog 133,98,37 into the beige 127,102,56).
    int const storm_idx = (curr_sky_param == CLEAR_WATER) ? STORM_WATER : STORM;
    SkyParam const* storm = skyParams[storm_idx];
    if (storm)
    {
      return glm::mix(base, colorFromParam(storm, r, t), g_weather_intensity);
    }
  }
  return base;
}

glm::vec3 Sky::colorFromParam(SkyParam const* sky_param, int r, int t) const
{
  if (!sky_param || r < 0 || r >= NUM_SkyColorNames || sky_param->mmin[r] < 0 || sky_param->colorRows[r].empty())
  {
    // [2026-08-22 WPL "water still blue"] WATER bands the zone left UNAUTHORED are ZERO in the
    // client (its band table defaults to black; e.g. the WPL murk lights 31/35 author their ocean
    // bands empty -> the client's bay water is near-black under the orange fog). The editor-palette
    // fallback below painted those bands a hardcoded blue (0.22,0.45,0.55) instead -- the "blue
    // lake" mismatch. Keep the palette for SKY bands (editor ergonomics on unauthored maps); the
    // four water bands follow the client.
    if (r == OCEAN_COLOR_LIGHT || r == OCEAN_COLOR_DARK || r == RIVER_COLOR_LIGHT || r == RIVER_COLOR_DARK)
    {
      return glm::vec3(0.0f);
    }
    return default_sky_color(r);
  }
  glm::vec3 c1, c2;
  int t1, t2;
  int last = static_cast<int>(sky_param->colorRows[r].size()) - 1;

  if (last == 0)
  {
      c1 = sky_param->colorRows[r][last].color;
      c2 = sky_param->colorRows[r][0].color;
      t1 = sky_param->colorRows[r][last].time;
      t2 = sky_param->colorRows[r][0].time + 2880;
      t += 2880;
  }
  else
  {
      if (t < sky_param->mmin[r])
      {
          // reverse interpolate
          c1 = sky_param->colorRows[r][last].color;
          c2 = sky_param->colorRows[r][0].color;
          t1 = sky_param->colorRows[r][last].time;
          t2 = sky_param->colorRows[r][0].time + 2880;
          t += 2880;
      }
      else
      {
          for (int i = last; true; i--)
          { //! \todo iterator this.
              if (sky_param->colorRows[r][i].time <= t)
              {
                  c1 = sky_param->colorRows[r][i].color;
                  t1 = sky_param->colorRows[r][i].time;

                  if (i == last)
                  {
                      c2 = sky_param->colorRows[r][0].color;
                      t2 = sky_param->colorRows[r][0].time + 2880;
                  }
                  else
                  {
                      c2 = sky_param->colorRows[r][i + 1].color;
                      t2 = sky_param->colorRows[r][i + 1].time;
                  }
                  break;
              }
          }
      }
  }

  float tt = static_cast<float>(t - t1) / static_cast<float>(t2 - t1);
  return c1*(1.0f - tt) + c2*tt;
}

const float rad = 400.0f;

// CLIENT-EXACT sky dome rows (wow.exe 5875 FUN_006d0d10, polar-angle table .rdata @0x81152c =
// pi*{0, .17, .20, .23, .24, .25} on a cap flattened by cos(45 deg) -> view elevations below) --
// and below the horizon the client fills EVERYTHING with the FOG color in one band (FUN_006d0f50
// tail writes color 7 for the whole sub-horizon half). The old noggit rows blended the horizon
// color down to -30 deg, which left the fog band visibly short of the true horizon line.
const math::degrees angles[] = { math::degrees (90.0f)
                               , math::degrees (16.8f)
                               , math::degrees (9.9f)
                               , math::degrees (3.7f)
                               , math::degrees (1.8f)
                               , math::degrees (0.0f)
                               , math::degrees (-90.0f)
                               };
const int skycolors[] = { 2, 3, 4, 5, 6, 7, 7 };
const int cnum = 7;
const int hseg = 32;


Skies::Skies(unsigned int mapid, Noggit::NoggitRenderContext context)
  : stars (ModelInstance("Environments\\Stars\\Stars.mdx", context))
  , _context(context)
{
  _map_id = mapid;

  for (int color_index = 0; color_index < NUM_SkyColorNames; ++color_index)
  {
    color_set[color_index] = default_sky_color(color_index);
  }

  bool has_global = false;
  for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
  {
    if (mapid == i->getUInt(LightDB::Map))
    {
      Sky s(i, _context);
      skies.push_back(s);
      numSkies++;

      if (s.pos == glm::vec3(0, 0, 0))
        has_global = true;
    }
  }

  if (!has_global)
  {
    for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
    {
      if (1 == i->getUInt(LightDB::ID))
      {
        Sky s(i, _context);
        skies.push_back(s);
        numSkies++;
        break;
      }
    }
  }

  if (numSkies == 0)
  {
    RawDBC light = load_raw_dbc("DBFilesClient\\Light.dbc");
    RawDBC light_params = load_raw_dbc("DBFilesClient\\LightParams.dbc");
    RawDBC light_skybox = load_raw_dbc("DBFilesClient\\LightSkybox.dbc");
    RawDBC light_int_band = load_raw_dbc("DBFilesClient\\LightIntBand.dbc");
    RawDBC light_float_band = load_raw_dbc("DBFilesClient\\LightFloatBand.dbc");
    std::vector<std::size_t> fallback_rows;

    if (light.valid && light.field_count > LightDB::DataIDs)
    {
      std::size_t const available_sky_params = std::min<std::size_t>(NUM_SkyParamsNames, light.field_count - LightDB::DataIDs);
      for (std::size_t row = 0; row < light.record_count; ++row)
      {
        if (light.word(row, LightDB::Map) != mapid)
        {
          if (light.word(row, LightDB::ID) == 1)
          {
            fallback_rows.push_back(row);
          }
          continue;
        }

        std::vector<SkyParam*> params;
        for (std::size_t param_index = 0; param_index < available_sky_params; ++param_index)
        {
          std::uint32_t param_id = light.word(row, LightDB::DataIDs + param_index);
          params.push_back(param_id ? make_raw_sky_param(param_id, light_params, light_skybox, light_int_band, light_float_band, _context) : nullptr);
        }

        Sky sky(static_cast<int>(light.word(row, LightDB::ID))
                , glm::vec3(light.number(row, LightDB::PositionX) / skymul, light.number(row, LightDB::PositionY) / skymul, light.number(row, LightDB::PositionZ) / skymul)
                , light.number(row, LightDB::RadiusInner) / skymul
                , light.number(row, LightDB::RadiusOuter) / skymul
                , params
                , _context);
        if (sky.pos == glm::vec3(0, 0, 0))
        {
          has_global = true;
        }
        skies.push_back(sky);
        numSkies++;
      }

      if (numSkies == 0 && !fallback_rows.empty())
      {
        std::size_t row = fallback_rows.front();
        std::vector<SkyParam*> params;
        for (std::size_t param_index = 0; param_index < available_sky_params; ++param_index)
        {
          std::uint32_t param_id = light.word(row, LightDB::DataIDs + param_index);
          params.push_back(param_id ? make_raw_sky_param(param_id, light_params, light_skybox, light_int_band, light_float_band, _context) : nullptr);
        }
        skies.emplace_back(static_cast<int>(light.word(row, LightDB::ID))
                           , glm::vec3(0.f, 0.f, 0.f)
                           , 0.f
                           , 0.f
                           , params
                           , _context);
        numSkies++;
        has_global = true;
      }

      LogError << "Turtle sky: raw DBC fallback Light records " << light.record_count
               << ", int bands " << light_int_band.record_count
               << ", float bands " << light_float_band.record_count
               << ", loaded " << numSkies << " for map " << mapid << std::endl;
    }
  }

  // sort skies from smallest to largest; global last.
  // smaller skies will have precedence when calculating weights to achieve smooth transitions etc.
  std::sort(skies.begin(), skies.end());

  build_zone_polygons(mapid);
  
  int skies_with_skyboxes = 0;
  for (Sky& sky : skies)
  {
    if (drawable_skybox_param(sky))
    {
      skies_with_skyboxes++;
    }
  }

  LogError << "Turtle sky: map " << mapid << " loaded " << numSkies
           << " light rows, " << skies_with_skyboxes << " with drawable skyboxes" << std::endl;

  _need_color_buffer_update = true;
}

void Skies::light_at(glm::vec3 const& pos, int time, glm::vec3* out_diffuse, glm::vec3* out_ambient) const
{
  Sky const* default_sky = nullptr;
  for (auto const& sky : skies)
  {
    if (sky.pos == glm::vec3(0, 0, 0))
    {
      default_sky = &sky;
      break;
    }
  }

  glm::vec3 diffuse(1.0f), ambient(1.0f);
  if (default_sky)
  {
    diffuse = default_sky->colorFor(LIGHT_GLOBAL_DIFFUSE, time);
    ambient = default_sky->colorFor(LIGHT_GLOBAL_AMBIENT, time);
  }

  // the client's zone-polygon lights come first (FUN_007f1360 blends the queued list before the heap)
  {
    std::vector<std::pair<int, float>> zone;
    zone_polygon_weights(pos, zone);
    for (auto const& entry : zone)
    {
      for (auto const& sky : skies)
      {
        if (sky.Id == entry.first)
        {
          diffuse = glm::mix(diffuse, sky.colorFor(LIGHT_GLOBAL_DIFFUSE, time), entry.second);
          ambient = glm::mix(ambient, sky.colorFor(LIGHT_GLOBAL_AMBIENT, time), entry.second);
          break;
        }
      }
    }
  }

  // far -> near so nearer volumes override, mirroring update_sky_colors' weighted mix order
  std::vector<Sky const*> ordered;
  ordered.reserve(skies.size());
  for (auto const& sky : skies)
  {
    if (&sky != default_sky && glm::distance(pos, sky.pos) <= sky.r2)
    {
      ordered.push_back(&sky);
    }
  }
  std::sort(ordered.begin(), ordered.end(), [&](Sky const* a, Sky const* b)
            { return glm::distance(pos, a->pos) > glm::distance(pos, b->pos); });

  for (auto const* sky : ordered)
  {
    float const dist = glm::distance(pos, sky->pos);
    float w = (sky->r2 - sky->r1) > 0.0f ? (sky->r2 - dist) / (sky->r2 - sky->r1) : 1.0f;
    if (dist <= sky->r1)
    {
      w = 1.0f;
    }
    w = std::clamp(w, 0.0f, 1.0f);
    diffuse = glm::mix(diffuse, sky->colorFor(LIGHT_GLOBAL_DIFFUSE, time), w);
    ambient = glm::mix(ambient, sky->colorFor(LIGHT_GLOBAL_AMBIENT, time), w);
  }

  *out_diffuse = diffuse;
  *out_ambient = ambient;
}

void Skies::build_zone_polygons(unsigned int mapid)
{
  _zone_polygons.clear();
  auto const* project = Noggit::Project::CurrentProject::get();
  if (!project || project->projectVersion == Noggit::Project::ProjectVersion::CLASSIC)
  {
    return; // the 1.12 client has no such table
  }
  for (ZoneLightPolygonDef const& def : ZONE_LIGHT_POLYGON_DEFS)
  {
    if (def.map != mapid)
    {
      continue;
    }
    ZoneLightPolygon poly;
    poly.light_id = def.light_id;
    poly.points = parse_zone_polygon(def.svg_path);
    if (poly.points.empty())
    {
      continue;
    }
    poly.min_x = poly.min_z = FLT_MAX;
    poly.max_x = poly.max_z = -FLT_MAX;
    for (glm::vec2 const& p : poly.points)
    {
      poly.min_x = std::min(poly.min_x, p.x);
      poly.min_z = std::min(poly.min_z, p.y);
      poly.max_x = std::max(poly.max_x, p.x);
      poly.max_z = std::max(poly.max_z, p.y);
    }
    poly.min_x -= ZONE_POLY_FADE;
    poly.min_z -= ZONE_POLY_FADE;
    poly.max_x += ZONE_POLY_FADE;
    poly.max_z += ZONE_POLY_FADE;
    _zone_polygons.push_back(std::move(poly));
  }
  if (!_zone_polygons.empty())
  {
    LogError << "Turtle sky: map " << mapid << " has " << _zone_polygons.size()
             << " client zone-light polygons" << std::endl;
  }
}

void Skies::zone_polygon_weights(glm::vec3 const& pos, std::vector<std::pair<int, float>>& out) const
{
  out.clear();
  for (ZoneLightPolygon const& poly : _zone_polygons)
  {
    if (out.size() >= ZONE_POLY_MAX_ACTIVE)
    {
      break;
    }
    // FUN_0077eed0: inclusive box test first (the box already carries the fade), then the polygon
    if (!(poly.min_x <= pos.x && poly.min_z <= pos.z && pos.x <= poly.max_x && pos.z <= poly.max_z))
    {
      continue;
    }
    float const sd = zone_polygon_signed_distance(poly.points, pos.x, pos.z);
    if (sd - ZONE_POLY_FADE < 0.f)
    {
      out.emplace_back(poly.light_id, zone_polygon_weight(sd));
    }
  }
}

void Skies::apply_zone_polygon_weights(glm::vec3 const& pos)
{
  for (Sky& sky : skies)
  {
    sky.zone_weight = 0.f;
    sky.zone_order = -1;
  }
  std::vector<std::pair<int, float>> zone;
  zone_polygon_weights(pos, zone);
  for (std::size_t i = 0; i < zone.size(); ++i)
  {
    for (Sky& sky : skies)
    {
      if (sky.Id == zone[i].first)
      {
        sky.zone_weight = zone[i].second;
        sky.zone_order = static_cast<int>(i);
        break;
      }
    }
  }
}

std::vector<std::pair<Sky*, float>> Skies::weighted_lights()
{
  std::vector<std::pair<Sky*, float>> out;
  std::vector<Sky*> zone;
  for (Sky& sky : skies)
  {
    if (sky.zone_weight > 0.f)
    {
      zone.push_back(&sky);
    }
  }
  std::sort(zone.begin(), zone.end(), [](Sky* a, Sky* b) { return a->zone_order < b->zone_order; });
  for (Sky* s : zone)
  {
    out.emplace_back(s, s->zone_weight);
  }
  for (Sky& sky : skies)
  {
    if (sky.weight > 0.f)
    {
      out.emplace_back(&sky, sky.weight);
    }
  }
  return out;
}

Sky* Skies::findSkyWeights(glm::vec3 pos)
{
  Sky* default_sky = nullptr;
  for (Sky& sky : skies)
  {
    sky.zone_weight = 0.f;
    sky.zone_order = -1;
  }

  for (auto& sky : skies)
  {
    if (sky.pos == glm::vec3(0, 0, 0))
    {
      default_sky = &sky;
      break;
    }
  }

  if (_area_light_id > 0)
  {
    for (auto& sky : skies)
    {
      sky.weight = 0.f;
    }

    for (auto& sky : skies)
    {
      if (sky.Id == _area_light_id)
      {
        sky.weight = 1.f;
        return default_sky ? default_sky : &sky;
      }
    }
  }

  std::sort(skies.begin(), skies.end(), [=](Sky& a, Sky& b)
  {
    return glm::distance(pos, a.pos) > glm::distance(pos, b.pos);
  });

  // The in-place sort above MOVED every Sky element, invalidating the `default_sky` pointer captured
  // before it -- it now aliases whichever light sorted into that slot (the NEAREST positional light after
  // a descending-distance sort). Left stale, `update_sky_colors` built the full-strength base color/fog
  // from that nearest light (fog collapse -> "fog fills the zone") and the weight loop below forced that
  // light's weight to 0 so `Skies::draw` skipped its skybox (wrong skybox / "holes in the sky"). This was
  // the Icecrown zone-boundary bug. Re-resolve the global/default light (pos==0) in the SORTED vector.
  default_sky = nullptr;
  for (auto& sky : skies)
  {
    if (sky.pos == glm::vec3(0, 0, 0))
    {
      default_sky = &sky;
      break;
    }
  }

  for (auto& sky : skies)
  {
    float distance_to_light = glm::distance(pos, sky.pos);

    if (default_sky == &sky || distance_to_light > sky.r2)
    {
      sky.weight = 0.f;
      continue;
    }

    float length_of_falloff = sky.r2 - sky.r1;
    sky.weight = (sky.r2 - distance_to_light) / length_of_falloff;

    if (distance_to_light <= sky.r1)
    {
      sky.weight = 1.0f;
    }

  }

  // [client RE 2026-09-09] the hardcoded zone polygons (camera x/z; the eval height plays no part)
  apply_zone_polygon_weights(pos);

  return default_sky;
}

Sky* Skies::findClosestSkyByWeight()
{
    // gets the highest weight sky
    if (skies.size() == 0)
        return nullptr;

    Sky* closest_sky = &skies[0];
    for (auto& sky : skies)
    {
        if (sky.weight > closest_sky->weight)
            closest_sky = &sky;
    }
    return closest_sky;
}

Sky* Skies::findClosestSkyByDistance(glm::vec3 pos)
{
    if (skies.size() == 0)
        return nullptr;

    Sky* closest = &skies[0];
    float distance = 1000000.f;
    for (auto& sky : skies)
    {
        float distanceToCenter = glm::distance(pos, sky.pos);

        if (distanceToCenter <= sky.r2 && distanceToCenter < distance)
        {
            distance = distanceToCenter;
            closest = &sky;
        }
    }

    return closest;
}

void Skies::setCurrentParam(int param_id)
{
    bool changed = false;
    for (auto& sky : skies)
    {
        if (sky.curr_sky_param != param_id)
        {
            sky.curr_sky_param = param_id;
            changed = true;
        }
    }
    // update_sky_colors caches on (_last_time,_last_pos) and would otherwise skip the recompute when
    // only the param changed (e.g. submerging while stationary). Force a refresh on an actual change.
    if (changed)
    {
        _last_time = -1;
    }
}

void Skies::setAreaLightId(int light_id)
{
  if (_area_light_id != light_id)
  {
    _area_light_id = light_id;
    _last_time = -1;
  }
}

void Skies::update_sky_colors(glm::vec3 pos, int time)
{
  if (numSkies == 0 || (_last_time == time && _last_pos == pos))
  {
    return;
  }  

  Sky* default_sky = findSkyWeights(pos);

  if (default_sky)
  {
    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      color_set[i] = default_sky->colorFor(i, time);
    }

    _fog_distance = default_sky->floatParamFor(0, time);
    _fog_multiplier = default_sky->floatParamFor(1, time);
    _cloud_coverage = default_sky->floatParamForAirVariant(CLOUD_DENSITY, time); // never the underwater band
    _celestial_flow = default_sky->floatParamFor(CELESTIAL_FLOW, time); // dusk twilight weight (was unread)

    auto default_sky_param = active_sky_param(*default_sky);
    if (default_sky_param)
    {
      _river_shallow_alpha = default_sky_param->river_shallow_alpha();
      _river_deep_alpha = default_sky_param->river_deep_alpha();
      _ocean_shallow_alpha = default_sky_param->ocean_shallow_alpha();
      _ocean_deep_alpha = default_sky_param->ocean_deep_alpha();
      _glow = default_sky_param->glow();
    }

  }
  else
  {
    LogError << "Failed to load default light. Something went seriously wrong. Potentially corrupt Light.dbc" << std::endl;

    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      color_set[i] = glm::vec3(1, 1, 1);
    }

    _fog_multiplier = 0.f;
    _fog_distance = 0.f;
    _cloud_coverage = 0.f;
    _celestial_flow = 0.f;

    _river_shallow_alpha = 0.f;
    _river_deep_alpha = 0.f;
    _ocean_shallow_alpha = 0.f;
    _ocean_deep_alpha = 0.f;
    _glow = 0.0f;

  }

  // interpolation -- the client's blend order (FUN_007f1360): the zone-POLYGON lights first (table order),
  // then every positional light farthest-first (`skies` is sorted that way by findSkyWeights). Each light is
  // lerped over the running result by its weight, so the nearest one wins.
  auto const blend_light = [&](Sky const& sky, float weight)
  {
    // now calculate the color rows
    for (int i = 0; i<NUM_SkyColorNames; ++i)
    {
      if ((sky.colorFor(i, time).x>1.0f) || (sky.colorFor(i, time).y>1.0f) || (sky.colorFor(i, time).z>1.0f))
      {
        LogDebug << "Sky " << sky.Id << " " << i << " is out of bounds!" << std::endl;
        continue;
      }
      auto timed_color = sky.colorFor(i, time);
      color_set[i] = glm::mix(color_set[i], timed_color, weight);
    }

    _fog_distance = (_fog_distance * (1.0f - weight)) + (sky.floatParamFor(0, time) * weight);
    _fog_multiplier = (_fog_multiplier * (1.0f - weight)) + (sky.floatParamFor(1, time) * weight);
    _cloud_coverage = (_cloud_coverage * (1.0f - weight)) + (sky.floatParamForAirVariant(CLOUD_DENSITY, time) * weight);
    _celestial_flow = (_celestial_flow * (1.0f - weight)) + (sky.floatParamFor(CELESTIAL_FLOW, time) * weight);
    auto sky_param = active_sky_param(sky);
    if (sky_param)
    {
      _river_shallow_alpha = (_river_shallow_alpha * (1.0f - weight)) + (sky_param->river_shallow_alpha() * weight);
      _river_deep_alpha = (_river_deep_alpha * (1.0f - weight)) + (sky_param->river_deep_alpha() * weight);
      _ocean_shallow_alpha = (_ocean_shallow_alpha * (1.0f - weight)) + (sky_param->ocean_shallow_alpha() * weight);
      _ocean_deep_alpha = (_ocean_deep_alpha * (1.0f - weight)) + (sky_param->ocean_deep_alpha() * weight);

      _glow = (_glow * (1.0f - weight)) + (sky_param->glow() * weight);
    }
  };
  std::vector<std::pair<Sky*, float>> const ordered_lights = weighted_lights();
  for (auto const& entry : ordered_lights)
  {
    blend_light(*entry.first, entry.second);
  }

  // Exterior WMO water flat colour = the WATER param's RIVER_COLOR_DARK band, weighted like every
  // other band (default light then positional mixes). Falls back to a light's active param when it
  // authors no CLEAR_WATER set. Published via the Skies static for wmo_liquid::draw.
  {
    glm::vec3 water_river_dark(0.0f);
    if (default_sky)
    {
      SkyParam const* p = default_sky->skyParams[CLEAR_WATER]
                        ? default_sky->skyParams[CLEAR_WATER]
                        : active_sky_param(*default_sky);
      if (p)
      {
        water_river_dark = default_sky->colorFromParam(p, RIVER_COLOR_DARK, time);
      }
    }
    for (auto const& entry : ordered_lights)
    {
      Sky const& sky = *entry.first;
      SkyParam const* p = sky.skyParams[CLEAR_WATER] ? sky.skyParams[CLEAR_WATER]
                                                     : active_sky_param(sky);
      if (p)
      {
        water_river_dark = glm::mix(water_river_dark,
                                    sky.colorFromParam(p, RIVER_COLOR_DARK, time), entry.second);
      }
    }
    Skies::set_water_river_dark(water_river_dark);
  }

  // The 1.12 client uses pure LINEAR vertex fog (D3DFOG_LINEAR), verified via apitrace on Elwynn. The
  // shader applies fogFactor = 1 - ((end-dist)/(end-start))^fog_rate, so fog_rate = 1.0 is the client's
  // straight linear ramp between fog start and end. (Was a 1.0-1.6 exponent tuned by eye, which read as
  // too-dense fog vs. the client.) fog_start/end distances are computed render-side (see WorldRender:
  // fog_end = min(fog_distance_end, view_distance); fog_start stays the DBC fraction of fog_end).
  // DIAGNOSTIC (NOGGIT_LIGHT_DEBUG): dump the zone-light fog selection so overblown-fog spots can be pinned.
  {
    static bool const s_zf_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
    static int s_zf_tick = 0;
    if (s_zf_dbg && (++s_zf_tick % 30) == 0)
    {
      int weighted = 0;
      float total_zone_weight = 0.f;
      for (auto const& s : skies) if (s.weight > 0.f || s.zone_weight > 0.f) { ++weighted; total_zone_weight += std::max(s.weight, s.zone_weight); }
      // LIGHTSEL: the resulting outdoor light colour + how much of it is the GLOBAL DEFAULT light.
      // total_zone_weight < 1 means the sequential blend leaves (1 - total) of the pos-(0,0,0) default
      // light in the mix -- and map 0's default (Light id 1, param 12) is a garish ORANGE sun by day
      // (LightIntBand band0 noon = (255,136,0)). So a warm/orange DIFFUSE here with low total_zone_weight
      // == the "exterior too warm" bleed (open/transition terrain not fully covered by a cool zone light).
      glm::vec3 const dbg_dif = color_set[LIGHT_GLOBAL_DIFFUSE];
      glm::vec3 const dbg_amb = color_set[LIGHT_GLOBAL_AMBIENT];
      LogError << "LIGHTSEL pos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
               << " default=" << (default_sky ? std::to_string(default_sky->Id) : std::string("NULL"))
               << " weightedSkies=" << weighted << " totalZoneWeight=" << total_zone_weight
               << " DIFFUSE=(" << dbg_dif.x << "," << dbg_dif.y << "," << dbg_dif.z << ")"
               << " AMBIENT=(" << dbg_amb.x << "," << dbg_amb.y << "," << dbg_amb.z << ")" << std::endl;
      LogError << "ZONEFOG pos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
               << " default=" << (default_sky ? std::to_string(default_sky->Id) : std::string("NULL"))
               << " fog_distance=" << _fog_distance << " fog_end=" << fog_distance_end()
               << " weightedSkies=" << weighted << std::endl;
      for (auto const& s : skies)
      {
        if (s.weight <= 0.f && s.zone_weight <= 0.f) continue;
        LogError << "  sky id=" << s.Id << " w=" << s.weight << " zoneW=" << s.zone_weight << " zoneOrder=" << s.zone_order
                 << " fogDist=" << s.floatParamFor(0, time)
                 << " pos=(" << s.pos.x << "," << s.pos.y << "," << s.pos.z << ")"
                 << " r1=" << s.r1 << " r2=" << s.r2 << std::endl;
      }
    }
  }

  // GUARD (overblown-fog fix): a bad/short-fog positioned light -- or the no-default fallback that wipes
  // colours to white and _fog_distance to 0 -- can collapse the blended zone fog distance to ~0, which fogs
  // the ENTIRE view to the (often white) sky colour. That is the "overblown fog" at Steelgrill's Depot and
  // over half of Icecrown. A zone (outdoor) fog under ~50yd is essentially always that failure (real short
  // fog lives in WMO MFOG, not the zone band), so fall back to the default light's authored fog distance
  // (or a sane 500yd) rather than whiting out. Root cause of WHICH light is bad is logged above.
  if (_fog_distance < 1800.f)
  {
    _fog_distance = (default_sky && default_sky->floatParamFor(0, time) >= 1800.f)
                      ? default_sky->floatParamFor(0, time)
                      : 18000.f;
  }

  _fog_rate = 1.0f;

  _last_pos = pos;
  _last_time = time;

  _need_color_buffer_update = true;  
}

namespace
{
  // Classic Perlin permutation table -- extracted verbatim from wow.exe 5875 .rdata @0x86f2d0
  // (the client's cloud lattice hash, FUN_006cffc0).
  std::uint8_t const CLOUD_PERM[256] = {
    225,155,210,108,175,199,221,144,203,116, 70,213, 69,158, 33,252,
      5, 82,173,133,222,139,174, 27,  9, 71, 90,246, 75,130, 91,191,
    169,138,  2,151,194,235, 81,  7, 25,113,228,159,205,253,134,142,
    248, 65,224,217, 22,121,229, 63, 89,103, 96,104,156, 17,201,129,
     36,  8,165,110,237,117,231, 56,132,211,152, 20,181,111,239,218,
    170,163, 51,172,157, 47, 80,212,176,250, 87, 49, 99,242,136,189,
    162,115, 44, 43,124, 94,150, 16,141,247, 32, 10,198,223,255, 72,
     53,131, 84, 57,220,197, 58, 50,208, 11,241, 28,  3,192, 62,202,
     18,215,153, 24, 76, 41, 15,179, 39, 46, 55,  6,128,167, 23,188,
    106, 34,187,140,164, 73,112,182,244,195,227, 13, 35, 77,196,185,
     26,200,226,119, 31,123,168,125,249, 68,183,230,177,135,160,180,
     12,  1,243,148,102,166, 38,238,251, 37,240,126, 64, 74,161, 40,
    184,149,171,178,101, 66, 29, 59,146, 61,254,107, 42, 86,154,  4,
    236,232,120, 21,233,209, 45, 98,193,114, 78, 19,206, 14,118,127,
     48, 79,147, 85, 30,207,219, 54, 88,234,190,122, 95, 67,143,109,
    137,214,145, 93, 92,100,245,  0,216,186, 60, 83,105, 97,204, 52,
  };
  // Per-octave fixed-point (8.8) lattice steps per texel for LOD 0 (128px), .rdata @0x86f3dc row 0:
  // {16,32,64,128} -> {8,16,32,64} lattice cells across the texture, weights 1, 1/2, 1/4, 1/8.
  int const CLOUD_OCTAVE_STEP[4] = { 16, 32, 64, 128 };
  // Dome row polar angles (x pi, 0 = zenith .. 0.25 = 45 deg) and per-row alphas, .rdata
  // @0x811570 / @0x8115a0. The dome is a cap flattened so its 45-deg edge sits at eye level:
  // vertex z = cos(theta) - cos(45 deg); the last rows fade the cap out just above the horizon.
  float const CLOUD_DOME_LAT[12] = { 0.f, 0.025f, 0.05f, 0.075f, 0.10f, 0.125f,
                                     0.15f, 0.175f, 0.205f, 0.230f, 0.245f, 0.25f };
}

void Skies::init_cloud_gen()
{
  auto& c = _clouds;
  c.rgba.assign(CloudGen::SIZE * CloudGen::SIZE * 4, 0);
  c.partial.assign(CloudGen::SIZE, 0.f);
  c.grad.assign(CloudGen::SIZE * CloudGen::SIZE, glm::vec2(0.f));
  c.prev_row.assign(CloudGen::SIZE, 0.f);
  // Client fills the lattice value table with rand()-based [-1,1] floats (FUN_006d0c90) -- the
  // pattern is session-random even in the real client, so any fixed seed is faithful.
  std::uint32_t s = 0x12345u;
  for (int i = 0; i < 256; ++i)
  {
    s = s * 214013u + 2531011u;
    c.value_table[i] = 1.f - static_cast<float>((s >> 16) & 0x7fff) * (2.f / 32767.f);
    c.ease[i] = (1.f - std::cos(static_cast<float>(i) * glm::pi<float>() / 256.f)) * 0.5f;
    // ramp built at init with the default 0.6 coverage (FUN_006d1ba0 -> FUN_006d0970(0.6) then
    // FUN_006d0900(0.96)): step = (255 - 102)/255 = 0.6; ramp[i] = 255 - 255 * 0.96^(0.6*i).
    c.ramp[i] = static_cast<std::uint8_t>(glm::clamp(
        255.f - 255.f * std::pow(0.96f, 0.6f * static_cast<float>(i)), 0.f, 255.f));
  }
  gl.genTextures(1, &c.texture);
  gl.bindTexture(GL_TEXTURE_2D, c.texture);
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CloudGen::SIZE, CloudGen::SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, c.rgba.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  c.initialized = true;
}

void Skies::tick_clouds(float dt_sec)
{
  auto& c = _clouds;
  c.timer -= dt_sec;
  if (c.timer > 0.f)
    return;
  c.timer = 0.1f; // regen period, .rdata @0x8115b4

  int const S = CloudGen::SIZE;
  // Coverage threshold: FUN_006d0970(band3) -> T = (1 - clamp01(coverage)) * 255. idx below T -> no cloud.
  int const T = static_cast<int>(std::lround((1.f - glm::clamp(_cloud_coverage, 0.f, 1.f)) * 255.f));

  // 3D value noise lattice hash (FUN_006cffc0): h(y,z) = perm[(perm[z] + y) & 255];
  // value(x) = value_table[perm[(h + x) & 255]].
  int const zc = (c.pass_counter >> 8) & 0xff;
  int const zh0 = CLOUD_PERM[zc];
  int const zh1 = CLOUD_PERM[(zc + 1) & 0xff];
  float const ez = c.ease[c.pass_counter & 0xff];

  // Per-texel sun lighting (FUN_006cfb00): light direction projected onto the cloud cap with the
  // client's LINEAR theta->radius map (FUN_006cf870), z offset 64 texels (storm-free: 0*192+64).
  glm::vec3 const ld = _celestial_dir;
  float const l_elev = std::asin(glm::clamp(ld.y, -1.f, 1.f));
  float const l_theta = glm::clamp(std::acos(glm::clamp(0.70710678f * std::cos(l_elev), -1.f, 1.f)) - l_elev,
                                   0.f, glm::quarter_pi<float>());
  float const l_rad = l_theta / glm::quarter_pi<float>() * 0.5f;
  glm::vec2 lh(ld.x, ld.z);
  float const lhl = glm::length(lh);
  lh = (lhl > 1e-5f) ? lh / lhl : glm::vec2(0.f);
  float const lx = (lh.x * l_rad + 0.5f) * static_cast<float>(S);
  float const ly = (lh.y * l_rad + 0.5f) * static_cast<float>(S);
  float const dz = 64.f;
  // Highlight strength: the client scales by the sun/moon HANDOFF curve (8-key table @0xce9ab8,
  // built by FUN_006ce390, evaluated by FUN_006cf6c0 in FUN_006cfb00): 1.0 for the WHOLE day
  // 05:30-21:30 (so the sun-behind-cloud glow stays at full power through sunset), fading to 0
  // only across the 04:00-05:30 / 21:30-22:10 celestial handoffs where the moon takes over at 1.0.
  static std::pair<float, float> const handoff_keys[] = {
    { 0.166667f, 1.0f }, // 04:00 moon full
    { 0.194444f, 0.0f }, // 04:40 moon faded out
    { 0.201389f, 0.0f }, // 04:50
    { 0.229167f, 1.0f }, // 05:30 sun full
    { 0.895833f, 1.0f }, // 21:30 sun still full
    { 0.923611f, 0.0f }, // 22:10 sun faded out
    { 0.888889f, 0.0f }, // (moon rise window, client stores these two out of order)
    { 0.916667f, 1.0f }, // 22:00 moon full
  };
  float const strength = sky_keyframe(handoff_keys, 8, glm::fract(static_cast<float>(_last_time) / 2880.f));

  glm::vec3 const c_high = color_set[SUN_HALO_COLOR];  // DBC color 10: sun-facing highlight
  glm::vec3 const c_tint = color_set[CLOUD_EDGE_COLOR]; // DBC color 11: density-shade tint
  glm::vec3 const c_base = color_set[CLOUD_COLOR];      // DBC color 12: dense-core base

  int const row0 = c.row_cursor;
  for (int y = row0; y < row0 + CloudGen::ROWS_PER_TICK; ++y)
  {
    // density: 4-octave fBm; capture the octave-0..2 partial per texel for the light gradient
    float prev_partial = 0.f;
    for (int x = 0; x < S; ++x)
    {
      float total = 0.f;
      float part = 0.f;
      for (int o = 0; o < 4; ++o)
      {
        int const step = CLOUD_OCTAVE_STEP[o];
        int const fx = (x * step) & 0xffff;
        int const fy = (y * step) & 0xffff;
        int const cx = fx >> 8, cy = fy >> 8;
        float const ex = c.ease[fx & 0xff];
        float const ey = c.ease[fy & 0xff];
        auto corner = [&](int hy, int hz) -> float {
          int const h = CLOUD_PERM[(hz + hy) & 0xff];
          float const v0 = c.value_table[CLOUD_PERM[(h + cx) & 0xff]];
          float const v1 = c.value_table[CLOUD_PERM[(h + cx + 1) & 0xff]];
          return v0 + (v1 - v0) * ex;
        };
        float const vz0 = glm::mix(corner(cy, zh0), corner(cy + 1, zh0), ey);
        float const vz1 = glm::mix(corner(cy, zh1), corner(cy + 1, zh1), ey);
        float const val = glm::mix(vz0, vz1, ez);
        total += val / static_cast<float>(1 << o);
        if (o == 2)
        {
          part = total;
          // gradient of the partial density, backward differences (FUN_006cffc0 octave-2 block);
          // scale 2^(shift-7) = 1 at LOD 0
          c.grad[y * S + x] = glm::vec2(prev_partial - part, c.prev_row[x] - part);
          prev_partial = part;
          c.prev_row[x] = part;
        }
      }
      // alpha: idx = byte(density*64 + 128) (.rdata 0x808de4/0x80653c); minus coverage threshold;
      // through the exponential ramp
      int const idx = glm::clamp(static_cast<int>(std::lround(total * 64.f + 128.f)), 0, 255);
      int const ri = idx - T;
      std::uint8_t const a = (ri < 0) ? 0 : c.ramp[glm::min(ri, 255)];

      std::uint8_t* px = &c.rgba[(y * S + x) * 4];
      if (a == 0)
      {
        // client copies the left neighbour's RGB for empty texels (keeps bilinear edges clean)
        if (x > 0) { px[0] = px[-4]; px[1] = px[-3]; px[2] = px[-2]; }
        px[3] = 0;
        continue;
      }
      // shade = ((255 - a) >> 1) + 64: thin cloud -> bright, dense core -> dark (FUN_006cfb00)
      float const s = static_cast<float>(((255 - a) >> 1) + 64) / 255.f;
      glm::vec3 rgb = c_base + c_tint * s;
      glm::vec2 const g = c.grad[y * S + x];
      float const dx = lx - static_cast<float>(x);
      float const dy = ly - static_cast<float>(y);
      float const dot = (dx * g.x + dy * g.y + dz)
                      / (std::sqrt(dx * dx + dy * dy + dz * dz) * std::sqrt(g.x * g.x + g.y * g.y + 1.f));
      if (dot > 0.f)
        rgb += c_high * (dot * strength);
      rgb = glm::clamp(rgb, 0.f, 1.f);
      px[0] = static_cast<std::uint8_t>(rgb.r * 255.f);
      px[1] = static_cast<std::uint8_t>(rgb.g * 255.f);
      px[2] = static_cast<std::uint8_t>(rgb.b * 255.f);
      px[3] = a;
    }
  }

  gl.bindTexture(GL_TEXTURE_2D, c.texture);
  ++_vk_cloud_tex_serial;   // [VULKAN] the deck texture changed -> VK re-uploads its bindless slot
  gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, row0, S, CloudGen::ROWS_PER_TICK,
                   GL_RGBA, GL_UNSIGNED_BYTE, &c.rgba[row0 * S * 4]);

  c.row_cursor += CloudGen::ROWS_PER_TICK;
  if (c.row_cursor >= S)
  {
    c.row_cursor = 0;
    ++c.pass_counter; // z axis: 1 lattice cell per 256 full passes (counter high byte)
  }
}

void Skies::draw_clouds(glm::mat4x4 const& mvp, glm::vec3 const& camera_pos, int animtime, bool covered)
{
  QSettings cs;
  bool const on = cs.value("render/draw_clouds", true).toBool();
  // suppressed while the camera is underwater: draw nothing, but the tick below still runs on
  // the AIR density -- the deck texture stays intact for the moment of surfacing.
  float const opacity = (on && !_cloud_draw_suppressed && !covered)
                      ? cs.value("render/cloud_density", 0.9f).toFloat() : 0.0f;

  if (!_clouds.initialized)
    init_cloud_gen();

  float dt = static_cast<float>(animtime - _last_cloud_animtime) * 0.001f;
  _last_cloud_animtime = animtime;
  if (on)
    tick_clouds(glm::clamp(dt, 0.f, 0.5f));
  _vk_cloud_opacity = opacity;   // [VULKAN] the deck's live strength, pushed to the VK pipeline
  if (opacity <= 0.001f)
    return;

  if (!_cloud_program)
  {
    _cloud_program.reset(new OpenGL::program(
      {
        {GL_VERTEX_SHADER, R"code(
#version 330 core
uniform mat4 model_view_projection;
uniform vec3 camera_pos;
in vec3 position;
in vec2 uv;
in float alpha;
out vec2 f_uv;
out float f_alpha;
void main()
{
  gl_Position = model_view_projection * vec4(position + camera_pos, 1.0);
  f_uv = uv;
  f_alpha = alpha;
}
)code"},
        {GL_FRAGMENT_SHADER, R"code(
#version 330 core
uniform sampler2D cloud_tex;
uniform float cloud_opacity;
in vec2 f_uv;
in float f_alpha;
out vec4 out_color;
void main()
{
  vec4 t = texture(cloud_tex, f_uv);
  out_color = vec4(t.rgb, t.a * f_alpha * cloud_opacity);
}
)code"}
      }));

    // ===== CLIENT CLOUD DOME MESH (wow.exe 5875 FUN_006d0530, tables .rdata 0x811570/0x8115a0):
    // pole vertex + 11 rings x 17 columns at polar angles pi*lat[i] on a cap flattened by
    // cos(45 deg) (edge at eye level), UV = (sin az, cos az) * (row/11 * 0.5) + 0.5, per-row
    // vertex alpha {255 x9, 128, 0, 0}. =====
    static float const lat[12] = { 0.f, 0.025f, 0.05f, 0.075f, 0.10f, 0.125f,
                                   0.15f, 0.175f, 0.205f, 0.230f, 0.245f, 0.25f };
    static float const row_alpha[12] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.502f, 0.f, 0.f };
    float const R = 400.0f; // same scale class as the sky sphere; camera-centred, depth writes off
    float const c45 = 0.70710678f;

    std::vector<float> verts; // pos3 uv2 alpha1
    auto push = [&](glm::vec3 const& p, glm::vec2 const& t, float a) {
      verts.insert(verts.end(), { p.x, p.y, p.z, t.x, t.y, a });
    };
    // pole
    push(glm::vec3(0.f, (1.f - c45) * R, 0.f), glm::vec2(0.5f, 0.5f), row_alpha[0]);
    for (int i = 1; i < 12; ++i)
    {
      float const th = glm::pi<float>() * lat[i];
      float const r = static_cast<float>(i) / 11.f * 0.5f;
      for (int j = 0; j <= 16; ++j)
      {
        float const az = static_cast<float>(j) * (glm::two_pi<float>() / 16.f);
        float const sa = std::sin(az), ca = std::cos(az);
        push(glm::vec3(std::sin(th) * sa * R, (std::cos(th) - c45) * R, std::sin(th) * ca * R),
             glm::vec2(sa * r + 0.5f, ca * r + 0.5f), row_alpha[i]);
      }
    }
    auto ring = [](int i, int j) -> std::uint16_t { return static_cast<std::uint16_t>(1 + (i - 1) * 17 + j); };
    std::vector<std::uint16_t> idx;
    for (int j = 0; j < 16; ++j) // pole fan
    {
      idx.push_back(0);
      idx.push_back(ring(1, j));
      idx.push_back(ring(1, j + 1));
    }
    for (int i = 1; i < 11; ++i)
      for (int j = 0; j < 16; ++j)
      {
        std::uint16_t const a = ring(i, j), b = ring(i, j + 1), c = ring(i + 1, j + 1), d = ring(i + 1, j);
        idx.insert(idx.end(), { a, b, c, a, c, d });
      }
    _cloud_indices_count = static_cast<int>(idx.size());

    gl.genVertexArrays(1, &_cloud_vao);
    gl.genBuffers(1, &_cloud_vbo);
    gl.genBuffers(1, &_cloud_ibo);
    {
      OpenGL::Scoped::use_program sh{*_cloud_program.get()};
      gl.bindVertexArray(_cloud_vao);
      gl.bindBuffer(GL_ARRAY_BUFFER, _cloud_vbo);
      gl.bufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
      sh.attrib("position", 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
      sh.attrib("uv", 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void const*>(3 * sizeof(float)));
      sh.attrib("alpha", 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void const*>(5 * sizeof(float)));
      gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _cloud_ibo);
      gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(std::uint16_t), idx.data(), GL_STATIC_DRAW);
      gl.bindVertexArray(0);

      // [VULKAN] mirror the cap for the VK cloud pipeline (GL's copies live only in these locals).
      _vk_cloud_verts = verts;
      _vk_cloud_indices = idx;
      _vk_cloud_mesh_dirty = true;
    }
  }

  OpenGL::Scoped::use_program shader{*_cloud_program.get()};
  shader.uniform("model_view_projection", mvp);
  shader.uniform("camera_pos", camera_pos);
  gl.activeTexture(GL_TEXTURE0);
  gl.bindTexture(GL_TEXTURE_2D, _clouds.texture);
  shader.uniform("cloud_tex", 0);
  shader.uniform("cloud_opacity", opacity);

  // alpha blend over the sky; keep the framebuffer ALPHA untouched (it is the bloom emissive
  // mask the sky wrote as 0). ZWRITE off, fog off, like the client (state block calls 6335-6339).
  gl.enable(GL_BLEND);
  gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const no_cull;
  OpenGL::Scoped::depth_mask_setter<GL_FALSE> const no_depth_write;
  if (!_vk_owns_clouds)
  {
    gl.bindVertexArray(_cloud_vao);
    gl.drawElements(GL_TRIANGLES, _cloud_indices_count, GL_UNSIGNED_SHORT, nullptr);
    gl.bindVertexArray(0);
  }
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl.disable(GL_BLEND);
}

bool Skies::draw(glm::mat4x4 const& model_view
                , glm::mat4x4 const& projection
                , glm::vec3 const& camera_pos
                , OpenGL::Scoped::use_program& m2_shader
                , math::frustum const& frustum
                , const float& cull_distance
                , int animtime
                , OutdoorLightStats const& light_stats
                )
{
  // Reset what this frame drew before the branches below refill it.
  _vk_skybox_instances.clear();
  _vk_stars_instance = nullptr;

  if (numSkies == 0)
  {
    if (!_uploaded)
    {
      upload();
    }

    if (_need_color_buffer_update)
    {
      update_color_buffer();
    }

    OpenGL::Scoped::use_program shader {*_program.get()};

    if(_need_vao_update)
    {
      update_vao(shader);
    }

    OpenGL::Scoped::vao_binder const _ (_vao);

    shader.uniform("model_view_projection", projection * model_view);
    shader.uniform("camera_pos", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));

    // The sky dome is an OPAQUE backdrop, but its fragment shader writes alpha 0 (bloom-mask opt-out). If
    // GL_BLEND is left enabled by a prior pass (the frame's last particle/additive draws do), SRC_ALPHA=0
    // makes the whole sky blend to fully transparent -> the cleared black shows through. Bloom hid this
    // because its composite disables blend as the last op. Force blend off for the opaque sky dome.
    gl.disable(GL_BLEND);
    if (!_vk_owns_dome)
      gl.drawElements(GL_TRIANGLES, _indices_count, GL_UNSIGNED_SHORT, nullptr);

    draw_clouds(projection * model_view, camera_pos, animtime);

    return true;
  }

  if (!_uploaded)
  {
    upload();
  }

  // ---- [client RE 2026-09-09, wow335a.exe] the frame's SKYBOX LIST, built BEFORE the dome because it
  // decides whether the dome is drawn at all.
  //  * Zone polygons (FUN_0077eed0 / FUN_007ed150 / FUN_007ee6b0): the client's hardcoded per-zone polygon
  //    table (ZONE_LIGHT_POLYGON_DEFS) queues its light with weight (50 - signed distance) / 100, blended
  //    FIRST -- Icecrown's whole zone gets Light 1703 (IceCrownSky) as a full-weight base this way.
  //  * Light blend (FUN_007f1360 / FUN_007ee5d0): then every light inside its outer radius, popped farthest-
  //    first, weight 1 inside r1 falling linearly to 0 at r2 -- what findSkyWeights/update_sky_colors do.
  //  * Slots (FUN_007ed4c0): a weighted light's LightParams.lightSkyboxId goes into one of THREE slots;
  //    the same LightSkybox id reached through several lights ADDS its weights (clamped to 1), otherwise
  //    the first free slot; a fourth distinct skybox is dropped.
  //  * List (FUN_007f3230): the slots are walked in order; a skybox above 0.99 weight whose LightSkybox
  //    flags lack 0x2 RESTARTS the list (it covers what came before); flag-0x2 skyboxes (the Northrend
  //    aurora models) are overlays that never restart it.
  //  * Draw (FUN_007f09b0): the stars model, the sun/moon billboards, the vertex-coloured gradient dome
  //    and the cloud dome are drawn ONLY when no listed skybox is a full-weight non-overlay; then every
  //    listed skybox in order with alpha = its weight (FUN_007f08c0 writes it into the model), a flag-0x1
  //    one at the time-of-day frame (FUN_007ecf20: dayMinutes / 1440 x anim length). Under a full skybox
  //    the background is the cleared frame, and the clear colour is the fog colour (light+0x8c).
  //  * Sun/moon brightness x (1 - the strongest listed weight) (FUN_007ef6e0): WorldRender reads
  //    skyboxCover() / skyboxCovers().
  // The earlier "Northrend = sky type 2" reading (grey fog, no celestials, a darkened dome) was
  // ScreenEffect.dbc's GHOST effect (FUN_004f7020 case 2) and is gone. WotLK projects only; the 1.12
  // path keeps the single highest-weight skybox over the dome.
  bool const wotlk_sky = []{
    auto const* p = Noggit::Project::CurrentProject::get();
    return p && p->projectVersion != Noggit::Project::ProjectVersion::CLASSIC;
  }();
  _skybox_list.clear();
  _skybox_cover = 0.f;
  _skybox_covers = false;
  if (wotlk_sky)
  {
    struct Slot { SkyParam* param = nullptr; float weight = 0.f; };
    Slot slot_arr[3]; // (not "slots": a Qt keyword macro)
    // zone-polygon lights first (table order), then the positional ones farthest-first = the client's
    // FUN_007f1360 blend order, which is the slot order
    for (auto const& entry : weighted_lights())
    {
      SkyParam* p = drawable_skybox_param(*entry.first);
      if (!p)
      {
        continue;
      }
      for (Slot& s : slot_arr)
      {
        if (s.param && s.param->skybox_id == p->skybox_id)
        {
          s.weight = std::min(1.f, s.weight + entry.second);
          break;
        }
        if (!s.param)
        {
          s.param = p;
          s.weight = entry.second;
          break;
        }
      }
    }
    for (Slot const& s : slot_arr)
    {
      if (!s.param || s.weight <= 0.f)
      {
        continue;
      }
      bool const overlay = (s.param->skybox_flags & 0x2) != 0;
      if (s.weight > 0.99f && !overlay)
      {
        _skybox_list.clear();
      }
      _skybox_list.push_back({&s.param->skybox.value(), s.param, s.weight, overlay});
    }
    for (SkyboxEntry const& e : _skybox_list)
    {
      _skybox_cover = std::max(_skybox_cover, e.weight);
      _skybox_covers = _skybox_covers || (e.weight > 0.99f && !e.overlay);
    }
  }
  else
  {
    Sky* top_sky = nullptr;
    SkyParam* top_param = nullptr;
    for (Sky& sky : skies)
    {
      SkyParam* sky_param = drawable_skybox_param(sky);
      if (sky.weight > 0.f && sky_param && sky_param->skybox && (sky_param->skybox_flags & 0x2) == 0
          && (!top_sky || sky.weight > top_sky->weight))
      {
        top_sky = &sky;
        top_param = sky_param;
      }
    }
    if (top_sky)
    {
      _skybox_list.push_back({&top_param->skybox.value(), top_param, top_sky->weight, false});
    }
  }

  // a full skybox replaces the dome: paint it flat in the fog colour = the client's cleared frame
  std::optional<glm::vec3> const flat = _skybox_covers ? std::optional<glm::vec3>(color_set[FOG_COLOR])
                                                       : std::nullopt;
  if (flat != _flat_dome_color)
  {
    _flat_dome_color = flat;
    _need_color_buffer_update = true;
  }
  if (_need_color_buffer_update)
  {
    update_color_buffer();
  }

  {
    OpenGL::Scoped::use_program shader {*_program.get()};

    if(_need_vao_update)
    {
      update_vao(shader);
    }

    {
      OpenGL::Scoped::vao_binder const _ (_vao);

      shader.uniform("model_view_projection", projection * model_view);
      shader.uniform("camera_pos", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));

      // Opaque sky dome, but the shader writes alpha 0 (bloom-mask opt-out). Blend left on by a prior
      // particle pass would make SRC_ALPHA=0 blend it fully transparent -> black sky (only visible with
      // bloom off, since bloom's composite disables blend last). Force blend off here.
      gl.disable(GL_BLEND);
      if (!_vk_owns_dome)
        gl.drawElements(GL_TRIANGLES, _indices_count, GL_UNSIGNED_SHORT, nullptr);
    }
  }

  // the cloud deck is not drawn under a full skybox (FUN_007efd00 / FUN_007f09b0); it keeps ticking
  draw_clouds(projection * model_view, camera_pos, animtime, _skybox_covers);

  for (SkyboxEntry const& e : _skybox_list)
  {
    ModelInstance& model = *e.model;
    SkyParam* sky_param = e.param;
    model.model->trans = e.weight;
    model.pos = camera_pos;
    model.scale = 0.1f;
    model.recalcExtents();

    // FULL-DAY SKYBOX (LightSkybox flag 0x1): the M2's single animation spans the whole day, so its
    // frame is driven by TIME OF DAY rather than a free-running clock (FUN_007ecf20 sets animation 0
    // at dayMinutes / 1440 x length). Map the day fraction (0 = midnight) onto anim 0's length;
    // Model::animate mods time by that length, so a value in [0, length) lands on the matching frame.
    // Other skyboxes keep the free-running animtime.
    int skybox_animtime = animtime;
    if (sky_param->skybox_flags & 0x1)
    {
      uint32_t const len = model.model->animationLength(0);
      if (len > 0)
      {
        float const day_frac = glm::fract(static_cast<float>(_last_time) / 2880.0f);
        skybox_animtime = static_cast<int>(day_frac * static_cast<float>(len)) % static_cast<int>(len);
      }
    }

    OpenGL::M2RenderState model_render_state;
    model_render_state.tex_arrays = {0, 0};
    model_render_state.tex_indices = {0, 0};
    model_render_state.tex_unit_lookups = {-1, -1};
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE);
    m2_shader.uniform("blend_mode", 0);
    m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
    m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
    m2_shader.uniform("tex_unit_lookup_1", 0);
    m2_shader.uniform("tex_unit_lookup_2", 0);
    m2_shader.uniform("masked_additive", 0);
    m2_shader.uniform("pixel_shader", 0);

    _vk_skybox_instances.push_back(&model);   // [VULKAN] fed to the VK M2 feed by WorldRender
    model.model->renderer()->draw(model_view, model, m2_shader, model_render_state, frustum, 1000000, camera_pos, skybox_animtime, display_mode::in_3D);
  }

  // Stars at night. CANON (wow_westfall_night.trace: the client draws a full star group -- textured
  // star sprites + an ~863-vert star sphere -- before the gradient dome). 3.3.5a never draws them under a
  // full-weight non-overlay skybox (FUN_007f09b0); the 1.12 path keeps its "any skybox drew" rule.
  // Kept behind render/draw_stars (default ON) as a toggle.
  bool const stars_blocked = wotlk_sky ? _skybox_covers : !_skybox_list.empty();
  if (light_stats.nightIntensity > 0 && !stars_blocked
      && QSettings().value("render/draw_stars", true).toBool())
  {
    stars.model->trans = light_stats.nightIntensity;
    stars.pos = camera_pos;
    stars.scale = 0.1f;
    stars.recalcExtents();

    OpenGL::M2RenderState model_render_state;
    model_render_state.tex_arrays = {0, 0};
    model_render_state.tex_indices = {0, 0};
    model_render_state.tex_unit_lookups = {-1, -1};
    gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.disable(GL_BLEND);
    gl.depthMask(GL_TRUE);
    m2_shader.uniform("blend_mode", 0);
    m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
    m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
    m2_shader.uniform("tex_unit_lookup_1", 0);
    m2_shader.uniform("tex_unit_lookup_2", 0);
    m2_shader.uniform("masked_additive", 0);
    m2_shader.uniform("pixel_shader", 0);

    _vk_stars_instance = &stars;   // [VULKAN] fed to the VK M2 feed by WorldRender
    stars.model->renderer()->draw(model_view, stars, m2_shader, model_render_state, frustum, 1000000, camera_pos, animtime, display_mode::in_3D);
  }

  return true;
}

void Skies::drawLightingSpheres (glm::mat4x4 const& model_view
  , glm::mat4x4 const& projection
  , glm::vec3 const& camera_pos
  , math::frustum const& frustum
  , const float& cull_distance
)
{
  for (Sky& sky : skies)
  {
    if (glm::distance(sky.pos, camera_pos) <= cull_distance) // TODO: frustum cull here
    {
        glm::vec4 diffuse = { color_set[LIGHT_GLOBAL_DIFFUSE], 1.f };
        glm::vec4 ambient = { color_set[LIGHT_GLOBAL_AMBIENT], 1.f };

        Log << sky.Id << " <=> (x,y,z) : " << sky.pos.x << "," << sky.pos.y << "," << sky.pos.z << " -- r1 : " << sky.r1 << " -- r2 : " << sky.r2 << std::endl;

        _sphere_render.draw(model_view * projection, sky.pos, ambient, sky.r1, 32, 18, 1.f);
        _sphere_render.draw(model_view * projection, sky.pos, diffuse, sky.r2, 32, 18, 1.f);
    }
  }
}

void Skies::drawLightingSphereHandles (glm::mat4x4 const& model_view
  , glm::mat4x4 const& projection
  , glm::vec3 const& camera_pos
  , math::frustum const& frustum
  , const float& cull_distance
  , bool draw_spheres)
{
  for (Sky& sky : skies)
  {
    if (glm::distance(sky.pos, camera_pos) - sky.r2 <= cull_distance) // TODO: frustum cull here
    {

      _sphere_render.draw(model_view * projection, sky.pos, {1.f, 0.f, 0.f, 1.f}, 5.f);

      if (sky.selected())
      {
        glm::vec3 diffuse = color_set[LIGHT_GLOBAL_DIFFUSE];
        glm::vec3 ambient = color_set[LIGHT_GLOBAL_AMBIENT];
        _sphere_render.draw(model_view * projection, sky.pos, {ambient.x, ambient.y, ambient.z, 0.3}, sky.r1);
        _sphere_render.draw(model_view * projection, sky.pos, {diffuse.x, diffuse.y, diffuse.z, 0.3}, sky.r2);
      }
    }
  }
}


void Skies::unload()
{
  _program.reset();
  _vertex_array.unload();
  _buffers.unload();
  _sphere_render.unload();

  if (_clouds.texture)
  {
    gl.deleteTextures(1, &_clouds.texture);
    _clouds.texture = 0;
  }
  _clouds.initialized = false;
  _cloud_program.reset();
  if (_cloud_vao)
  {
    gl.deleteVertexArray(1, &_cloud_vao);
    gl.deleteBuffers(1, &_cloud_vbo);
    gl.deleteBuffers(1, &_cloud_ibo);
    _cloud_vao = _cloud_vbo = _cloud_ibo = 0;
    _cloud_indices_count = 0;
  }

  _uploaded = false;
  _need_vao_update = true;

}

void Skies::upload()
{
  _program.reset(new OpenGL::program(
    {
        {GL_VERTEX_SHADER, R"code(
#version 330 core

uniform mat4 model_view_projection;
uniform vec3 camera_pos;

in vec3 position;
in vec3 color;

out vec3 f_color;
out vec3 f_dir;

void main()
{
  vec4 pos = vec4(position + camera_pos, 1.f);
  gl_Position = model_view_projection * pos;
  f_color = color;
  f_dir = normalize(position); // dome direction for the cloud layer
}
)code" }
        , {GL_FRAGMENT_SHADER, R"code(
#version 330 core

in vec3 f_color;
in vec3 f_dir;

out vec4 out_color;

void main()
{
  // Pure gradient dome -- the cloud layer is separate GEOMETRY (Skies::draw_clouds), exactly like
  // the client: a real mesh with baked polar UVs, so triangle interpolation shapes the zenith.
  vec3 col = f_color;
  // Alpha = bloom mask; write 0 so the sky opts out of the emissive bloom range (bright cloud/sun
  // still bloom via their own luminance through the FFXGlow blur^2 composite).
  out_color = vec4(col, 0.);
}
)code" }
    }
  ));

  _vertex_array.upload();
  _buffers.upload();

  std::vector<glm::vec3> vertices;
  std::vector<std::uint16_t> indices;

  glm::vec3 basepos1[cnum], basepos2[cnum];

  for (int h = 0; h < hseg; h++)
  {
    for (int i = 0; i < cnum; ++i)
    {
      basepos1[i] = basepos2[i] = glm::vec3(glm::cos(math::radians(angles[i])._) * rad, glm::sin(math::radians(angles[i])._)*rad, 0);

      math::rotate(0, 0, &basepos1[i].x, &basepos1[i].z, math::radians(glm::pi<float>() *2.0f / hseg * h));
      math::rotate(0, 0, &basepos2[i].x, &basepos2[i].z, math::radians(glm::pi<float>() *2.0f / hseg * (h + 1)));
    }

    for (int v = 0; v < cnum - 1; v++)
    {
      int start = static_cast<int>(vertices.size());

      vertices.push_back(basepos2[v]);
      vertices.push_back(basepos1[v]);
      vertices.push_back(basepos1[v + 1]);
      vertices.push_back(basepos2[v + 1]);

      indices.push_back(start+0);
      indices.push_back(start+1);
      indices.push_back(start+2);

      indices.push_back(start+2);
      indices.push_back(start+3);
      indices.push_back(start+0);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STATIC_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STATIC_DRAW);

  // [VULKAN] mirror the dome for the VK sky pipeline (GL's copies live only in these locals).
  _vk_dome_verts = vertices;
  _vk_dome_indices = indices;
  _vk_sky_dirty = true;

  _indices_count = static_cast<int>(indices.size());

  _uploaded = true;
  _need_vao_update = true;
}

void Skies::update_vao(OpenGL::Scoped::use_program& shader)
{
  OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_vbo);

  {
    OpenGL::Scoped::vao_binder const _ (_vao);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> vertices_buffer (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> colors_buffer (_colors_vbo);
    shader.attrib("color", 3, GL_FLOAT, GL_FALSE, 0, 0);

    indices_binder.bind();
  }

  _need_vao_update = false;
}

// Dusk/dawn twilight dome curves -- CLIENT-CANON, dumped from wow.exe 5875 (FUN_006ce120 @0xce9b2c,
// FUN_006ce210 @0xce9af8; consumed by the dome builder FUN_006d0f50). NOT hand-tuned.
// TWILIGHT envelope over day fraction (0=00:00): dawn pulse ~05:30, dusk pulse ~21:30, 0->1->0.
static std::pair<float, float> const twilight_time_keys[] = {
  { 0.125000f, 0.0f }, { 0.270833f, 1.0f }, { 0.291667f, 0.0f },   // dawn (center 0.229167)
  { 0.854167f, 0.0f }, { 0.895833f, 1.0f }, { 0.999306f, 0.0f },   // dusk (center 0.895833)
};
// AZIMUTH glow shape over the per-column phase local_14: NEGATIVE = sun-facing glow branch (|value| =
// strength), 0..1 positive = plain branch (1 => keep raw band). Peak glow -0.7 at phase 0.625.
static std::pair<float, float> const azimuth_glow_keys[] = {
  { 0.125f, 1.0f }, { 0.375f, 0.0f }, { 0.5f, -0.5f }, { 0.625f, -0.7f }, { 0.75f, -0.5f }, { 0.875f, 0.0f },
};

void Skies::update_color_buffer()
{
  // CLIENT dome builder FUN_006d0f50 (byte-exact). The dome bands (colorFor) already interpolate to
  // dark-blue night verbatim; the two TWILIGHT terms below are what the client adds so dusk/dawn read
  // as a warm directional gradient instead of a flat near-black dome. Both vanish when w==0 (daytime,
  // deep night, or a map that doesn't author CELESTIAL_FLOW) -- then this reduces to the old flat dome.
  std::vector<glm::vec3> colors;
  colors.reserve(static_cast<std::size_t>(hseg) * (cnum - 1) * 4);

  // w = twilight-envelope(dayFrac) * CELESTIAL_FLOW (LightFloatBand 2). local_24 in FUN_006d0f50.
  float const dayFrac = glm::fract(static_cast<float>(_last_time) / 2880.f);
  float const w = sky_keyframe(twilight_time_keys, 6, dayFrac) * _celestial_flow;

  // Per-ring twilight pre-blend: pull bands 3..6 toward band 3 (SKY_COLOR_1) by w (FUN_006d0f50 phase 2).
  glm::vec3 scratch[NUM_SkyColorNames];
  for (int b = 0; b < NUM_SkyColorNames; ++b) { scratch[b] = color_set[b]; }
  if (w > 0.f)
  {
    for (int b = SKY_COLOR_1; b <= SKY_COLOR_4; ++b)
    {
      scratch[b] = glm::mix(color_set[b], color_set[SKY_COLOR_1], w);
    }
  }

  // Per-column phase: local_14 = sunBearing/(2pi) + 0.25, stepped by -1/columns (FUN_006d0f50). Sun
  // azimuth is fixed 225deg so the glow sits at a fixed compass and only w animates; anchor to
  // _celestial_dir (render frame, x-z) so it tracks the drawn sun.
  float const two_pi = glm::two_pi<float>();
  float const sun_az = std::atan2(_celestial_dir.z, _celestial_dir.x);
  float const l14_start = sun_az / two_pi + 0.25f;
  float const l14_step = -1.0f / static_cast<float>(hseg);

  auto ring_col_color = [&](int v, int col) -> glm::vec3
  {
    if (_flat_dome_color)
    {
      // a full-weight skybox replaces the dome: what shows through its transparent layers is the
      // client's cleared frame, and the clear colour is the fog colour (light+0x8c).
      return *_flat_dome_color;
    }
    int const band = skycolors[v];
    if (w <= 0.f || band == SKY_COLOR_0 || band == FOG_COLOR)
    {
      return color_set[band]; // zenith + horizon/fog rings: flat, no twilight (matches the client)
    }
    float const l14 = glm::fract(l14_start + l14_step * static_cast<float>(col));
    float const g = sky_keyframe(azimuth_glow_keys, 6, l14);
    glm::vec3 out;
    if (g < 0.f) // sun-facing GLOW: bend the pre-blended band toward the zenith band by w*0.7, weight |g|*w
    {
      glm::vec3 const zmix = glm::mix(scratch[band], color_set[SKY_COLOR_0], w * 0.7f);
      out = glm::mix(scratch[band], zmix, -g * w);
    }
    else // plain: raw band toward the pre-blended scratch by (1-g)*w
    {
      out = glm::mix(color_set[band], scratch[band], (1.0f - g) * w);
    }
    return glm::clamp(out, 0.f, 1.f);
  };

  for (int h = 0; h < hseg; h++)
  {
    for (int v = 0; v < cnum - 1; v++)
    {
      colors.push_back(ring_col_color(v,     h + 1)); // vtx0 = basepos2[v]   (col h+1, ring v)
      colors.push_back(ring_col_color(v,     h    )); // vtx1 = basepos1[v]   (col h,   ring v)
      colors.push_back(ring_col_color(v + 1, h    )); // vtx2 = basepos1[v+1] (col h,   ring v+1)
      colors.push_back(ring_col_color(v + 1, h + 1)); // vtx3 = basepos2[v+1] (col h+1, ring v+1)
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_colors_vbo, colors, GL_STATIC_DRAW);

  // [VULKAN] same colours feed the VK dome pipeline.
  _vk_dome_colors = colors;
  _vk_sky_dirty = true;

  _need_vao_update = true;
}


// Circular piecewise-linear keyframe interpolation over (day-fraction, value) pairs -- the exact
// algorithm of the 1.12 client's celestial-path evaluator (wow.exe 5875 FUN_006cf6c0): find the
// first key with time > t, lerp from the previous key with midnight wrap-around.
float sky_keyframe(std::pair<float, float> const* keys, int count, float t)
{
  t = glm::clamp(t, 0.f, 1.f);
  int next = 0;
  while (next < count && keys[next].first <= t)
    ++next;
  int const prev = (next == 0 || next == count) ? count - 1 : next - 1;
  if (next == count)
    next = 0;
  float span = keys[next].first - keys[prev].first;
  if (std::abs(span) < 1e-5f)
    return keys[prev].second;
  if (span < 0.f)
    span += 1.f;
  float f = t - keys[prev].first;
  if (f < 0.f)
    f += 1.f;
  return glm::mix(keys[prev].second, keys[next].second, f / span);
}

void OutdoorLightStats::interpolate(OutdoorLightStats *a, OutdoorLightStats *b, float r)
{
  static constexpr unsigned DayNight_SecondsPerDay = 86400;

  float progressDayAndNight = r / DayNight_SecondsPerDay;

  // SCENE LIGHT direction -- CLIENT-CANON (wow.exe 5875 FUN_006d3a10): the 1.12 scene light does NOT
  // travel across the sky; it oscillates gently between polar 110 deg (1.9198623) and 127 deg
  // (2.2165682) at a FIXED AZIMUTH = pi*1.25 = 225 deg, day and night. The azimuth is the constant
  // azimuth table (_DAT_00811508 = pi, times 1.25); the polar is the oscillating table [pi*0.705556,
  // pi*0.611111] = [127,110] deg -- matched exactly by phiTable below. RE 2026-07-26.
  //
  // AZIMUTH (2026-07-26): pi*1.25 (225 deg) is the CLIENT value. A prior change (2026-07-18) set this
  // to pi/4 (45 deg) to make the lit side agree with noggit's sun DISC -- but that flipped the scene
  // light 180 deg OPPOSITE the client, so terrain/WMO/M2 lit the WRONG compass side (the Stormwind-
  // harbor mountain, lit in-game, stayed dark at every time of day -- the light direction never
  // reached it). Restored to the client's 225 deg. The sun DISC (WorldRender celestial_dir) was
  // already correct at quarter_pi/45 deg and is LEFT there -- its celestial_dir uses a different frame
  // (a +180 flip + a different swizzle than this scene-light path), so 45 for the disc and 225 here
  // both land client-correct. (Flipping the disc to 225 to "match" put the visible sun 180 deg wrong.)
  float phiValue = 0;
  const float thetaValue = 3.926990817f; // pi * 1.25 == 225 deg (CLIENT canon, FUN_006d3a10 azimuth table)
  const float phiTable[4] =
    {
      2.2165682f,
      1.9198623f,
      2.2165682f,
      1.9198623f
    };

  unsigned currentPhiIndex = static_cast<unsigned>(progressDayAndNight / 0.25f);
  unsigned nextPhiIndex = 0;

  if (currentPhiIndex < 3)
    nextPhiIndex = currentPhiIndex + 1;

  // Lerp between the current value of phi and the next value of phi
  {
    float transitionProgress = (progressDayAndNight / 0.25f) - currentPhiIndex;

    float currentPhiValue = phiTable[currentPhiIndex];
    float nextPhiValue = phiTable[nextPhiIndex];

    phiValue = glm::mix(currentPhiValue, nextPhiValue, transitionProgress);
  }

  // Convert from Spherical Position to Cartesian coordinates
  float sinPhi = glm::sin(phiValue);
  float cosPhi = glm::cos(phiValue);

  float sinTheta = glm::sin(thetaValue);
  float cosTheta = glm::cos(thetaValue);

  dayDir.x = sinPhi * cosTheta;
  dayDir.y = sinPhi * sinTheta;
  dayDir.z = cosPhi;

  float ir = 1.0f - progressDayAndNight;
  nightIntensity = a->nightIntensity * ir + b->nightIntensity * progressDayAndNight;
}

OutdoorLighting::OutdoorLighting()
{

  static constexpr std::array<int, 24> night_hours =
    {1, 1, 1, 1, 1, 1,
     0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 1, 1};

  for (int i = 0; i < 24; ++i)
  {
    OutdoorLightStats ols;
    ols.nightIntensity = night_hours[i];
    lightStats.push_back(ols);
  }
}

OutdoorLightStats OutdoorLighting::getLightStats(int time)
{
  // ASSUME: only 24 light info records, one for each whole hour
  //! \todo  generalize this if the data file changes in the future

  int normalized_time ((static_cast<int>(time) % 2880) / 2);

  static constexpr unsigned DayNight_SecondsPerDay = 86400;

  long progressDayAndNight = (static_cast<float>(normalized_time) * 120);

  while (progressDayAndNight < 0 || progressDayAndNight > DayNight_SecondsPerDay)
  {
    if (progressDayAndNight > DayNight_SecondsPerDay)
      progressDayAndNight -= DayNight_SecondsPerDay;

    if (progressDayAndNight < 0)
      progressDayAndNight += DayNight_SecondsPerDay;
  }

  OutdoorLightStats out;

  OutdoorLightStats *a, *b;
  int ta = normalized_time / 60;
  int tb = (ta + 1) % 24;

  a = &lightStats[ta];
  b = &lightStats[tb];

  out.interpolate(a, b, progressDayAndNight);

  return out;
}

void Sky::save_to_dbc()
{
    // Save Light.dbc record
    // find new empty ID : gLightDB.getEmptyRecordID(); .prob do it when creating new light instead.
    DBCFile::Record data = is_new_record ? gLightDB.addRecord(Id) : gLightDB.getByID(Id);

    // pos = glm::vec3(data->getFloat(LightDB::PositionX) / skymul, data->getFloat(LightDB::PositionY) / skymul, data->getFloat(LightDB::PositionZ) / skymul);
    // record.write(1, _curr_sky-> map id
    data.write(LightDB::PositionX, pos.x * skymul);
    data.write(LightDB::PositionY, pos.y * skymul);
    data.write(LightDB::PositionZ, pos.z * skymul);
    data.write(LightDB::RadiusInner, r1 * skymul);
    data.write(LightDB::RadiusOuter,r2 * skymul);
    // data.write(7, Params Id TODO only needed for new entries

    // save LightParams.dbc
    // TODO : all params, not just clear.
    for (int param_id = 0; param_id < NUM_SkyFloatParamsNames; param_id++)
    {
        // skip if no param
        if (skyParams[param_id] == nullptr)
            continue;

        // TODO : several lights can use the same param, ask user if he wants to save a copy or edit it for all ?
        int lightParam_dbc_id = 0;
        if (is_new_record) // not for duplicates
            lightParam_dbc_id = gLightParamsDB.getEmptyRecordID();
        else
            lightParam_dbc_id = data.getInt(LightDB::DataIDs + param_id);

        if (lightParam_dbc_id == 0)
            continue;

        int light_int_start = lightParam_dbc_id * NUM_SkyColorNames - 17;

        for (int i = 0; i < NUM_SkyColorNames; ++i)
        {
            try
            {
                DBCFile::Record rec = is_new_record ? gLightIntBandDB.addRecord(light_int_start + i) : gLightIntBandDB.getByID(light_int_start + i);
                // int entries = rec.getInt(LightIntBandDB::Entries);
                int entries = static_cast<int>(skyParams[param_id]->colorRows[i].size());

                rec.write(LightIntBandDB::Entries, entries); // nb of entries

                for (int l = 0; l < 16; l++)
                {
                    if (l >= entries)
                    {
                        rec.write(LightIntBandDB::Times + l, 0);
                        rec.write(LightIntBandDB::Values + l, 0);
                    }
                    else
                    {
                        rec.write(LightIntBandDB::Times + l, skyParams[param_id]->colorRows[i][l].time);
                        
                        int rebuilt_color_int = static_cast<int>(skyParams[param_id]->colorRows[i][l].color.z * 255.0f)
                            + (static_cast<int>(skyParams[param_id]->colorRows[i][l].color.y * 255.0f) << 8)
                            + (static_cast<int>(skyParams[param_id]->colorRows[i][l].color.x * 255.0f) << 16);
                        rec.write(LightIntBandDB::Values + l, rebuilt_color_int);
                    }
                }
            }
            catch (...)
            {
                LogError << "When trying to intialize sky " << data.getInt(LightDB::ID) << ", there was an error with getting an entry in gLightIntBand (" << i << "). Sorry." << std::endl;
            }
        }

        int light_float_start = lightParam_dbc_id * NUM_SkyFloatParamsNames - 5;

        for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
        {
            try
            {
                DBCFile::Record rec = is_new_record ? gLightFloatBandDB.addRecord(light_float_start + i) : gLightFloatBandDB.getByID(light_float_start + i);
                int entries = static_cast<int>(skyParams[param_id]->floatParams[i].size());

                rec.write(LightFloatBandDB::Entries, entries); // nb of entries

                // for (int l = 0; l < entries; l++)
                for (int l = 0; l < 16; l++)
                {
                    if (l >= entries)
                    {
                        rec.write(LightFloatBandDB::Times + l, 0);
                        rec.write(LightFloatBandDB::Values + l, 0.0f);
                    }
                    else
                    {
                        rec.write(LightFloatBandDB::Times + l, skyParams[param_id]->floatParams[i][l].time);
                        rec.write(LightFloatBandDB::Values + l, skyParams[param_id]->floatParams[i][l].value);
                    }
                }
            }
            catch (...)
            {
                LogError << "When trying to intialize sky " << data.getInt(LightDB::ID) << ", there was an error with getting an entry in LightFloatBand (" << i << "). Sorry." << std::endl;
            }
        }

        try
        {
            DBCFile::Record light_param = gLightParamsDB.getByID(lightParam_dbc_id);

            if (skybox.has_value()) // TODO skybox dbc
            {
                // light_param.write(LightParamsDB::skybox, TODO);
            }
            else
                light_param.write(LightParamsDB::skybox, 0);

            light_param.write(LightParamsDB::highlightSky, int(skyParams[param_id]->highlight_sky()));
            light_param.write(LightParamsDB::water_shallow_alpha, skyParams[param_id]->river_shallow_alpha());
            light_param.write(LightParamsDB::water_deep_alpha, skyParams[param_id]->river_deep_alpha());
            light_param.write(LightParamsDB::ocean_shallow_alpha, skyParams[param_id]->ocean_shallow_alpha());
            light_param.write(LightParamsDB::ocean_deep_alpha, skyParams[param_id]->ocean_deep_alpha());
            light_param.write(LightParamsDB::glow, skyParams[param_id]->glow());
        }
        catch (...)
        {
            LogError << "When trying to get the skybox for the entry " << lightParam_dbc_id << " in LightParams.dbc. Sad." << std::endl;
        }

    }

    gLightDB.save();
    gLightIntBandDB.save();
    gLightFloatBandDB.save();
    gLightParamsDB.save();
    gLightSkyboxDB.save();

    // emit map_dbc_updated();

    is_new_record = false;


}
