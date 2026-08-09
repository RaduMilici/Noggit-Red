// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql/mysql.h>
#include <mysql.h>

#include <QtCore/QSettings>
#include <QMessageBox>
#include <noggit/MySqlSettings.hpp>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	struct ConnectionDetails
  {
		std::string host;
		std::string user;
		std::string password;
		std::string schema;
		unsigned int port = 3306;
	};

	ConnectionDetails loadConnectionDetails()
	{
		// Per-project MySQL settings (see MySqlSettings.hpp): keyed by the active project so a 3.3.5a
		// project and a Turtle project keep separate connections.
		ConnectionDetails details;
		details.host = Noggit::mysqlSetting("server", "127.0.0.1").toString().toStdString();
		details.user = Noggit::mysqlSetting("user", "root").toString().toStdString();
		details.password = Noggit::mysqlSetting("pwd", "mangos").toString().toStdString();
		details.schema = Noggit::mysqlSetting("db", "tw_world").toString().toStdString();
		details.port = Noggit::mysqlSetting("port", 3306).toUInt();

		return details;
	}

	bool executeStatement(MYSQL* connection, std::string const& statement, std::string* error = nullptr)
	{
		if (mysql_query(connection, statement.c_str()) == 0)
		{
			return true;
		}

		if (error)
		{
			*error = mysql_error(connection);
		}

		return false;
	}

	struct ConnectionCloser
	{
		void operator()(MYSQL* connection) const
		{
			if (connection)
			{
				mysql_close(connection);
			}
		}
	};

	std::unique_ptr<MYSQL, ConnectionCloser> connect(std::string* error = nullptr)
	{
		// Honor the per-project "MySQL enabled" toggle. Without this, every DB call (e.g. the creature/
		// gameobject model pickers built on map open) connects regardless of the toggle -- so a project
		// with MySQL DISABLED but a stale/unreachable host (e.g. Ascension: enabled=false, server=
		// 192.168.1.28) blocks the main thread in mysql_real_connect on every map open and freezes the
		// editor. If the user turned MySQL off for this project, do not connect at all.
		// DEFAULT FALSE: MySQL stays OFF unless the project has EXPLICITLY enabled it. Every other call site
		// (MapView, map_index, NoggitWindow) already defaults to false; this one defaulted to TRUE, so a
		// project that never configured MySQL would try to connect (and freeze / break the user's setup).
		if (!Noggit::mysqlSetting("enabled", false).toBool())
		{
			if (error)
			{
				*error = "MySQL is disabled for this project.";
			}
			return nullptr;
		}

		auto details = loadConnectionDetails();

		MYSQL* connection = mysql_init(nullptr);
		if (!connection)
		{
			if (error)
			{
				*error = "mysql_init failed";
			}
			return nullptr;
		}

		unsigned int const timeout_seconds = 5;
		mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds);
		mysql_options(connection, MYSQL_OPT_READ_TIMEOUT, &timeout_seconds);
		mysql_options(connection, MYSQL_OPT_WRITE_TIMEOUT, &timeout_seconds);

		if (!mysql_real_connect(connection,
														details.host.c_str(),
														details.user.c_str(),
														details.password.c_str(),
														nullptr,
														details.port,
														nullptr,
														0))
		{
			if (error)
			{
				*error = mysql_error(connection);
			}
			mysql_close(connection);
			return nullptr;
		}

		mysql_set_character_set(connection, "utf8");

		std::string create_database = "CREATE DATABASE IF NOT EXISTS `" + details.schema + "`";
		if (!executeStatement(connection, create_database, error))
		{
			mysql_close(connection);
			return nullptr;
		}

		if (mysql_select_db(connection, details.schema.c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection);
			}
			mysql_close(connection);
			return nullptr;
		}

		if (!executeStatement(connection,
													"CREATE TABLE IF NOT EXISTS `UIDs` ("
													"`_map_id` int(11) NOT NULL,"
													"`UID` int(11) NOT NULL,"
													"PRIMARY KEY(`_map_id`)"
													") ENGINE=InnoDB DEFAULT CHARSET=latin1;",
													error))
		{
			mysql_close(connection);
			return nullptr;
		}

		return std::unique_ptr<MYSQL, ConnectionCloser>(connection);
	}

	std::uint32_t parseUnsigned(char const* value)
	{
		if (!value)
		{
			return 0;
		}

		return static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
	}

	std::int32_t parseSigned(char const* value)
	{
		if (!value)
		{
			return 0;
		}

		return static_cast<std::int32_t>(std::strtol(value, nullptr, 10));
	}

	float parseFloat(char const* value)
	{
		if (!value)
		{
			return 0.0f;
		}

		return std::strtof(value, nullptr);
  }

	std::string parseString(char const* value)
	{
		return value ? std::string(value) : std::string();
	}

	std::string escapeString(MYSQL* connection, std::string const& value)
	{
		std::string escaped;
		escaped.resize(value.size() * 2 + 1);
		auto length = mysql_real_escape_string(connection, escaped.data(), value.c_str(), static_cast<unsigned long>(value.size()));
		escaped.resize(length);
		return escaped;
	}

	bool tableHasColumn(MYSQL* connection, char const* table_name, char const* column_name)
	{
		std::stringstream statement;
		statement << "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS "
		          << "WHERE TABLE_SCHEMA = DATABASE() "
		          << "AND TABLE_NAME='" << table_name << "' "
		          << "AND COLUMN_NAME='" << column_name << "' LIMIT 1";

		if (mysql_query(connection, statement.str().c_str()) != 0)
		{
			return false;
		}

		MYSQL_RES* result = mysql_store_result(connection);
		if (!result)
		{
			return false;
		}

		bool const has_row = mysql_num_rows(result) > 0;
		mysql_free_result(result);
		return has_row;
	}

	bool tableExists(MYSQL* connection, char const* table_name)
	{
		std::stringstream statement;
		statement << "SELECT 1 FROM INFORMATION_SCHEMA.TABLES "
		          << "WHERE TABLE_SCHEMA = DATABASE() "
		          << "AND TABLE_NAME='" << table_name << "' LIMIT 1";

		if (mysql_query(connection, statement.str().c_str()) != 0)
		{
			return false;
		}

		MYSQL_RES* result = mysql_store_result(connection);
		if (!result)
		{
			return false;
		}

		bool const has_row = mysql_num_rows(result) > 0;
		mysql_free_result(result);
		return has_row;
	}

	// Held-weapon (creature equipment) schema, tolerant of BOTH namings: TrinityCore/AC
	// (equipment_id / item_template.display_id / inventory_type) AND CMaNGOS (EquipmentTemplateId /
	// item_template.displayid / InventoryType). Without the CMaNGOS variants, cm_world resolves NO
	// weapons even though creature_equip_template + item_template hold valid data (Defias Blackguard
	// 636 -> equip 134 -> item 5285 -> displayid 6469).
	struct CreatureEquipmentSchema
	{
		bool valid = false;
		std::string ct_equip_col;   // creature_template equipment ref column
		std::string it_display_col; // item_template display column
		std::string it_invtype_col; // item_template inventory-type column
	};

	CreatureEquipmentSchema resolveCreatureEquipmentSchema(MYSQL* connection)
	{
		CreatureEquipmentSchema schema;

		if (tableHasColumn(connection, "creature_template", "equipment_id"))
			schema.ct_equip_col = "equipment_id";
		else if (tableHasColumn(connection, "creature_template", "EquipmentTemplateId"))
			schema.ct_equip_col = "EquipmentTemplateId";
		else
			return schema;

		if (!tableExists(connection, "creature_equip_template")
		    || !tableHasColumn(connection, "creature_equip_template", "entry")
		    || !tableHasColumn(connection, "creature_equip_template", "equipentry1")
		    || !tableHasColumn(connection, "creature_equip_template", "equipentry2")
		    || !tableHasColumn(connection, "creature_equip_template", "equipentry3")
		    || !tableExists(connection, "item_template")
		    || !tableHasColumn(connection, "item_template", "entry"))
			return schema;

		if (tableHasColumn(connection, "item_template", "display_id"))
			schema.it_display_col = "display_id";
		else if (tableHasColumn(connection, "item_template", "displayid"))
			schema.it_display_col = "displayid";
		else
			return schema;

		if (tableHasColumn(connection, "item_template", "inventory_type"))
			schema.it_invtype_col = "inventory_type";
		else if (tableHasColumn(connection, "item_template", "InventoryType"))
			schema.it_invtype_col = "InventoryType";
		else
			return schema;

		schema.valid = true;
		return schema;
	}

	std::string creatureEquipmentSelectExpr(CreatureEquipmentSchema const& schema)
	{
		if (!schema.valid)
		{
			return "0 AS mainhand_display_id, 0 AS offhand_display_id, 0 AS ranged_display_id, "
			       "0 AS mainhand_inventory_type, 0 AS offhand_inventory_type, 0 AS ranged_inventory_type";
		}

		std::string const& d = schema.it_display_col;
		std::string const& iv = schema.it_invtype_col;
		return "COALESCE(it1." + d + ", 0) AS mainhand_display_id, "
		       "COALESCE(it2." + d + ", 0) AS offhand_display_id, "
		       "COALESCE(it3." + d + ", 0) AS ranged_display_id, "
		       "COALESCE(it1." + iv + ", 0) AS mainhand_inventory_type, "
		       "COALESCE(it2." + iv + ", 0) AS offhand_inventory_type, "
		       "COALESCE(it3." + iv + ", 0) AS ranged_inventory_type";
	}

	std::string creatureEquipmentJoinExpr(CreatureEquipmentSchema const& schema)
	{
		if (!schema.valid)
		{
			return {};
		}

		return "LEFT JOIN creature_equip_template cet ON cet.entry = ct." + schema.ct_equip_col + " "
		       "LEFT JOIN item_template it1 ON it1.entry = cet.equipentry1 "
		       "LEFT JOIN item_template it2 ON it2.entry = cet.equipentry2 "
		       "LEFT JOIN item_template it3 ON it3.entry = cet.equipentry3 ";
	}

	std::string buildCreatureDisplayExpr(MYSQL* connection, bool* needs_creature_addon_join, bool* has_mount_display_col, bool* needs_template_model_join = nullptr)
	{
		std::vector<std::string> parts;
		std::vector<std::string> ambiguous_spawn_parts;
		parts.reserve(8);
		ambiguous_spawn_parts.reserve(2);

		// Prefer explicit spawn-level display overrides when available.
		if (tableHasColumn(connection, "creature", "displayid"))  parts.emplace_back("NULLIF(c.displayid, 0)");
		if (tableHasColumn(connection, "creature", "display_id")) parts.emplace_back("NULLIF(c.display_id, 0)");

		// Some world schemas use modelid/model_id for spawn display overrides, while others store
		// creature entry/template model references there. Treat those as a last-resort fallback so
		// template display_id1..4 remains authoritative for entries like Deathknight Captain 16145.
		if (tableHasColumn(connection, "creature", "modelid"))    ambiguous_spawn_parts.emplace_back("NULLIF(c.modelid, 0)");
		if (tableHasColumn(connection, "creature", "model_id"))   ambiguous_spawn_parts.emplace_back("NULLIF(c.model_id, 0)");

		bool const has_ca_display = tableHasColumn(connection, "creature_addon", "display_id");
		bool const has_ca_mount = tableHasColumn(connection, "creature_addon", "mount_display_id");

		if (has_ca_display)
		{
			parts.emplace_back("NULLIF(ca.display_id, 0)");
		}

		// creature_template inline display columns. TrinityCore / some schemas name them
		// display_id1..4 (underscore); CMaNGOS/mangos name them DisplayId1..4 (NO underscore). Detect
		// which naming THIS world DB uses so CMaNGOS worlds (e.g. cm_world) resolve creature displays
		// too -- without this every creature falls through to display_id 0 (verified: cm_world has
		// valid DisplayId1..4 data but the old snake_case-only check missed it). Gameobjects already
		// did this via firstColumnExpr; creatures didn't. Turtle (tw_source: display_id1) and
		// AzerothCore (creature_template_model) paths are unaffected.
		bool const has_display_id_snake = tableHasColumn(connection, "creature_template", "display_id1")
		                               && tableHasColumn(connection, "creature_template", "display_id2")
		                               && tableHasColumn(connection, "creature_template", "display_id3")
		                               && tableHasColumn(connection, "creature_template", "display_id4");
		bool const has_display_id_camel = tableHasColumn(connection, "creature_template", "DisplayId1")
		                               && tableHasColumn(connection, "creature_template", "DisplayId2")
		                               && tableHasColumn(connection, "creature_template", "DisplayId3")
		                               && tableHasColumn(connection, "creature_template", "DisplayId4");
		bool const has_template_display_ids = has_display_id_snake || has_display_id_camel;
		if (has_template_display_ids)
		{
			std::string const d1 = has_display_id_snake ? "ct.display_id1" : "ct.DisplayId1";
			std::string const d2 = has_display_id_snake ? "ct.display_id2" : "ct.DisplayId2";
			std::string const d3 = has_display_id_snake ? "ct.display_id3" : "ct.DisplayId3";
			std::string const d4 = has_display_id_snake ? "ct.display_id4" : "ct.DisplayId4";
			std::string const display_count = "((" + d1 + " > 0) + (" + d2 + " > 0) + (" + d3 + " > 0) + (" + d4 + " > 0))";
			std::string const first_display = "COALESCE(NULLIF(" + d1 + ", 0), NULLIF(" + d2 + ", 0), NULLIF(" + d3 + ", 0), NULLIF(" + d4 + ", 0))";
			std::string const second_display = "CASE "
				"WHEN (" + d1 + " > 0) + (" + d2 + " > 0) = 2 THEN " + d2 + " "
				"WHEN (" + d1 + " > 0) + (" + d2 + " > 0) + (" + d3 + " > 0) = 2 THEN " + d3 + " "
				"WHEN (" + d1 + " > 0) + (" + d2 + " > 0) + (" + d3 + " > 0) + (" + d4 + " > 0) = 2 THEN " + d4 + " END";
			std::string const third_display = "CASE "
				"WHEN (" + d1 + " > 0) + (" + d2 + " > 0) + (" + d3 + " > 0) = 3 THEN " + d3 + " "
				"WHEN (" + d1 + " > 0) + (" + d2 + " > 0) + (" + d3 + " > 0) + (" + d4 + " > 0) = 3 THEN " + d4 + " END";
			std::string const fourth_display = "CASE WHEN " + display_count + " = 4 THEN " + d4 + " END";

			std::stringstream template_display_expr;
			template_display_expr << "CASE WHEN " << display_count << " > 0 THEN CASE 1 + MOD(c.guid, GREATEST(" << display_count << ", 1)) "
			                      << "WHEN 1 THEN " << first_display << " "
			                      << "WHEN 2 THEN " << second_display << " "
			                      << "WHEN 3 THEN " << third_display << " "
			                      << "WHEN 4 THEN " << fourth_display << " END END";
			parts.emplace_back(template_display_expr.str());
		}

		parts.insert(parts.end(), ambiguous_spawn_parts.begin(), ambiguous_spawn_parts.end());

		// AzerothCore / TrinityCore 3.3.5a moved creature models out of creature_template into
		// creature_template_model (CreatureDisplayID per CreatureID/Idx). Use it as the final display
		// fallback -- on those schemas it's the ONLY source (creature_template has no inline display).
		bool const has_template_model = tableExists(connection, "creature_template_model")
		                             && tableHasColumn(connection, "creature_template_model", "CreatureDisplayID");
		if (has_template_model)
		{
			parts.emplace_back("NULLIF(ctm.CreatureDisplayID, 0)");
		}
		if (needs_template_model_join)
		{
			*needs_template_model_join = has_template_model;
		}

		if (needs_creature_addon_join)
		{
			*needs_creature_addon_join = has_ca_display || has_ca_mount;
		}
		if (has_mount_display_col)
		{
			*has_mount_display_col = has_ca_mount;
		}

		if (parts.empty())
		{
			return "0 AS displayid";
		}

		std::stringstream expr;
		expr << "COALESCE(";
		for (std::size_t i = 0; i < parts.size(); ++i)
		{
			expr << parts[i];
			if (i + 1 < parts.size())
			{
				expr << ", ";
			}
		}
		expr << ") AS displayid";
		return expr.str();
	}

	std::string firstTemplateColumnExpr(MYSQL* connection,
	                                    std::initializer_list<char const*> column_names,
	                                    char const* fallback)
	{
		for (auto const* column_name : column_names)
		{
			if (tableHasColumn(connection, "creature_template", column_name))
			{
				return std::string("ct.") + column_name;
			}
		}

		return fallback;
	}

	std::string firstColumnExpr(MYSQL* connection,
	                            char const* table_name,
	                            char const* alias,
	                            std::initializer_list<char const*> column_names,
	                            char const* fallback)
	{
		for (auto const* column_name : column_names)
		{
			if (tableHasColumn(connection, table_name, column_name))
			{
				return std::string(alias) + "." + column_name;
			}
		}

		return fallback;
	}

	std::string buildCreatureTemplateDisplayExpr(MYSQL* connection)
	{
		std::vector<std::string> parts;
		for (auto const* column_name : {"display_id1", "displayid1",
		                                "display_id2", "displayid2",
		                                "display_id3", "displayid3",
		                                "display_id4", "displayid4"})
		{
			if (tableHasColumn(connection, "creature_template", column_name))
			{
				parts.emplace_back(std::string("NULLIF(ct.") + column_name + ", 0)");
			}
		}

		if (parts.empty())
		{
			return "0 AS display_id";
		}

		std::stringstream expr;
		expr << "COALESCE(";
		for (std::size_t i = 0; i < parts.size(); ++i)
		{
			expr << parts[i];
			if (i + 1 < parts.size())
			{
				expr << ", ";
			}
		}
		expr << ", 0) AS display_id";
		return expr.str();
	}

	std::string buildGameObjectDisplayExpr(MYSQL* connection)
	{
		std::vector<std::string> parts;
		for (auto const* column_name : {"displayId", "displayid", "display_id", "displayID"})
		{
			if (tableHasColumn(connection, "gameobject_template", column_name))
			{
				parts.emplace_back(std::string("NULLIF(gt.") + column_name + ", 0)");
			}
		}

		if (parts.empty())
		{
			return "0 AS display_id";
		}

		std::stringstream expr;
		expr << "COALESCE(";
		for (std::size_t i = 0; i < parts.size(); ++i)
		{
			expr << parts[i];
			if (i + 1 < parts.size())
			{
				expr << ", ";
			}
		}
		expr << ", 0) AS display_id";
		return expr.str();
	}
}

namespace mysql
{
  bool testConnection(bool report_only_err)
  {
		std::string error;
		auto connection = connect(&error);
		if (connection)
		{
			if (!report_only_err)
			{
				QMessageBox prompt;
				prompt.setWindowFlag(Qt::WindowStaysOnTopHint);
				prompt.setIcon(QMessageBox::Information);
				prompt.setText("Successfully connected to MySQL database.");
				prompt.setWindowTitle("Success");
				prompt.exec();
			}

			return true;
		}

		QMessageBox prompt;
		prompt.setWindowFlag(Qt::WindowStaysOnTopHint);
		prompt.setIcon(QMessageBox::Warning);
		prompt.setText("Failed to load MySQL database, check your settings. \nIf you did not intend to use this feature, disable it in Noggit->settings->MySQL");
		prompt.setWindowTitle("Noggit Database Error");
		prompt.setInformativeText(error.c_str());
		prompt.exec();

		return false;
  }

  bool hasMaxUIDStoredDB(std::size_t mapID)
  {
		auto connection = connect();
		if (!connection)
		{
			return false;
		}

		std::stringstream statement;
		statement << "SELECT `UID` FROM `UIDs` WHERE `_map_id`=" << mapID << " LIMIT 1";
		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			return false;
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			return false;
		}

		bool has_row = mysql_num_rows(result) > 0;
		mysql_free_result(result);
		return has_row;
  }

  std::uint32_t getGUIDFromDB(std::size_t mapID)
  {
	  auto connection = connect();
	  if (!connection)
	  {
		  return 0;
	  }

	  std::stringstream statement;
	  statement << "SELECT `UID` FROM `UIDs` WHERE `_map_id`=" << mapID << " LIMIT 1";
	  if (mysql_query(connection.get(), statement.str().c_str()) != 0)
	  {
		  return 0;
	  }

	  MYSQL_RES* result = mysql_store_result(connection.get());
	  if (!result)
	  {
		  return 0;
	  }

	  MYSQL_ROW row = mysql_fetch_row(result);
	  std::uint32_t highGUID = row ? parseUnsigned(row[0]) : 0;
	  mysql_free_result(result);

	  return highGUID;
  }

  void insertUIDinDB(std::size_t mapID, std::uint32_t NewUID)
  {
	  auto connection = connect();
	  if (!connection)
	  {
		  return;
	  }

	  std::stringstream statement;
	  statement << "INSERT INTO `UIDs` (`_map_id`, `UID`) VALUES (" << mapID << ", " << NewUID << ")";
	  mysql_query(connection.get(), statement.str().c_str());
  }

  void updateUIDinDB (std::size_t mapID, std::uint32_t NewUID)
  {
	  auto connection = connect();
	  if (!connection)
	  {
		  return;
	  }

	  std::stringstream statement;
	  statement << "UPDATE `UIDs` SET `UID`=" << NewUID << " WHERE `_map_id`=" << mapID;
	  mysql_query(connection.get(), statement.str().c_str());
	}

	std::vector<CreatureSpawnRecord> getCreatureSpawns(std::size_t mapID, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		bool needs_creature_addon_join = false;
		bool needs_template_model_join = false;
		bool has_mount_display_col = false;
		bool has_template_scale_col = tableHasColumn(connection.get(), "creature_template", "scale");
		// AzerothCore/TrinityCore 3.3.5a keep the creature's scale in creature_template_model.DisplayScale
		// (creature_template has no scale column there).
		bool const has_template_model_scale = tableExists(connection.get(), "creature_template_model")
		                                   && tableHasColumn(connection.get(), "creature_template_model", "DisplayScale");
		auto const equipment_schema = resolveCreatureEquipmentSchema(connection.get());
		auto display_expr = buildCreatureDisplayExpr(connection.get(), &needs_creature_addon_join, &has_mount_display_col, &needs_template_model_join);
		auto mount_expr = has_mount_display_col
			? "COALESCE(CASE WHEN ca.mount_display_id > 0 THEN ca.mount_display_id ELSE 0 END, 0) AS mount_display_id"
			: "0 AS mount_display_id";

		// NPC POSE (stand-state + emote-state) from creature_addon, so a spawn plays its authored sit/sleep/
		// kneel/emote instead of Stand. Turtle & CMaNGOS expose stand_state directly; AzerothCore/TrinityCore
		// pack it into bytes1 (low byte = UnitStandState). Emote column: Turtle=emote_state, CMaNGOS/AC=emote.
		bool const has_ca_standstate = tableHasColumn(connection.get(), "creature_addon", "stand_state");
		bool const has_ca_bytes1     = !has_ca_standstate && tableHasColumn(connection.get(), "creature_addon", "bytes1");
		bool const has_ca_emotestate = tableHasColumn(connection.get(), "creature_addon", "emote_state");
		bool const has_ca_emote      = !has_ca_emotestate && tableHasColumn(connection.get(), "creature_addon", "emote");
		std::string const standstate_expr =
			  has_ca_standstate ? "COALESCE(ca.stand_state, 0) AS standstate"
			: has_ca_bytes1     ? "COALESCE(ca.bytes1 & 0xFF, 0) AS standstate"
			:                     "0 AS standstate";
		std::string const emotestate_expr =
			  has_ca_emotestate ? "COALESCE(ca.emote_state, 0) AS emotestate"
			: has_ca_emote      ? "COALESCE(ca.emote, 0) AS emotestate"
			:                     "0 AS emotestate";
		// Force the creature_addon join if it is only needed for pose (mount/display may be absent from ca).
		needs_creature_addon_join = needs_creature_addon_join
			|| has_ca_standstate || has_ca_bytes1 || has_ca_emotestate || has_ca_emote;

		// Spawn entry column: AzerothCore/TrinityCore 3.3.5a name it id1 (with id2/id3 for variants) and
		// have no plain `id`; Turtle/mangos use `id`. Pick whichever exists.
		std::string const creature_entry_col =
			  tableHasColumn(connection.get(), "creature", "id")  ? "c.id"
			: tableHasColumn(connection.get(), "creature", "id1") ? "c.id1"
			: "c.id";

		// Preserve a raw 0 (don't coerce to 1): creature_template.scale of 0 means "use the
		// CreatureDisplayInfo scale" (server ObjectMgr.cpp:1436). On 3.3.5a there's no ct.scale, so fall
		// back to creature_template_model.DisplayScale.
		std::string template_scale_expr;
		if (has_template_scale_col && has_template_model_scale)
			template_scale_expr = "COALESCE(NULLIF(ct.scale, 0), ctm.DisplayScale, 0) AS template_scale";
		else if (has_template_scale_col)
			template_scale_expr = "COALESCE(ct.scale, 0) AS template_scale";
		else if (has_template_model_scale)
			template_scale_expr = "COALESCE(ctm.DisplayScale, 0) AS template_scale";
		else
			template_scale_expr = "0 AS template_scale";

		// The ctm join is needed if either the display or the scale comes from creature_template_model.
		bool const needs_ctm_join = needs_template_model_join || has_template_model_scale;

		// Permanent aura spell ids (for aura state-kit visuals + the creature-info UI). Turtle/mangos
		// keep them in creature_template.auras; AzerothCore in creature_template_addon.auras.
		bool const has_template_auras = tableHasColumn(connection.get(), "creature_template", "auras");
		bool const has_template_addon_auras = !has_template_auras
		                                   && tableExists(connection.get(), "creature_template_addon")
		                                   && tableHasColumn(connection.get(), "creature_template_addon", "auras");
		auto const spawn_faction_expr = firstTemplateColumnExpr(connection.get(),
			{"faction", "faction_A", "faction_a", "factionAlliance", "factionHorde"}, "0");
		std::string const auras_expr = has_template_auras       ? "COALESCE(ct.auras, '') AS auras"
		                             : has_template_addon_auras ? "COALESCE(cta.auras, '') AS auras"
		                             :                            "'' AS auras";

		// Seasonal game-event membership. Turtle/mangos: game_event_creature(guid, event) where event is
		// signed (negative = "spawn EXCEPT while the event is active"). AzerothCore/TrinityCore name the
		// column eventEntry. Aggregate to one row per guid (a subquery join, so a guid in multiple event
		// rows never multiplies the spawn) and default 0 when the schema/link is absent.
		bool const has_event_creature = tableExists(connection.get(), "game_event_creature");
		std::string const event_col =
			  !has_event_creature ? ""
			: tableHasColumn(connection.get(), "game_event_creature", "event")      ? "event"
			: tableHasColumn(connection.get(), "game_event_creature", "eventEntry") ? "eventEntry"
			:                                                                          "";
		bool const join_event = has_event_creature && !event_col.empty();
		std::string const event_select = join_event ? "COALESCE(gec.gevent, 0) AS event" : "0 AS event";
		std::string const event_join = join_event
			? "LEFT JOIN (SELECT guid, MIN(" + event_col + ") AS gevent FROM game_event_creature GROUP BY guid) gec ON gec.guid = c.guid "
			: "";

		std::stringstream statement;
		statement
			<< "SELECT c.guid, " << creature_entry_col << ", c.map, c.position_x, c.position_y, c.position_z, c.orientation, ct.name, "
			<< template_scale_expr << ", "
			<< display_expr << ", "
			<< mount_expr << ", "
			<< creatureEquipmentSelectExpr(equipment_schema) << ", "
			<< auras_expr << ", "
			<< "COALESCE(" << spawn_faction_expr << ", 0) AS spawn_faction, "
			<< event_select << ", "
			<< standstate_expr << ", "
			<< emotestate_expr << " "
			<< "FROM creature c "
			<< "INNER JOIN creature_template ct ON ct.entry = " << creature_entry_col << " "
			<< (needs_ctm_join ? "LEFT JOIN creature_template_model ctm ON ctm.CreatureID = ct.entry AND ctm.Idx = 0 " : "")
			<< (needs_creature_addon_join ? "LEFT JOIN creature_addon ca ON ca.guid = c.guid " : "")
			<< (has_template_addon_auras ? "LEFT JOIN creature_template_addon cta ON cta.entry = ct.entry " : "")
			<< creatureEquipmentJoinExpr(equipment_schema)
			<< event_join
			<< "WHERE c.map = " << mapID << " "
			<< "ORDER BY ct.name, c.guid";

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<CreatureSpawnRecord> records;
		records.reserve(static_cast<std::size_t>(mysql_num_rows(result)));

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			CreatureSpawnRecord record;
			record.guid = parseUnsigned(row[0]);
			record.entry = parseUnsigned(row[1]);
			record.map = parseUnsigned(row[2]);
			record.position_x = parseFloat(row[3]);
			record.position_y = parseFloat(row[4]);
			record.position_z = parseFloat(row[5]);
			record.orientation = parseFloat(row[6]);
			record.name = parseString(row[7]);
			record.template_scale = parseFloat(row[8]);
			record.display_id = parseUnsigned(row[9]);
			record.mount_display_id = parseUnsigned(row[10]);
			record.mainhand_display_id = parseUnsigned(row[11]);
			record.offhand_display_id = parseUnsigned(row[12]);
			record.ranged_display_id = parseUnsigned(row[13]);
			record.mainhand_inventory_type = parseUnsigned(row[14]);
			record.offhand_inventory_type = parseUnsigned(row[15]);
			record.ranged_inventory_type = parseUnsigned(row[16]);
			record.auras = parseString(row[17]);
			record.faction = parseUnsigned(row[18]);
			record.event = parseSigned(row[19]);
			record.stand_state = static_cast<std::uint8_t>(parseUnsigned(row[20]));
			record.emote_state = parseUnsigned(row[21]);
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
  }

	std::map<std::uint32_t, SpellInfoRecord> getSpellInfos(std::set<std::uint32_t> const& spell_ids, std::string* error)
	{
		std::map<std::uint32_t, SpellInfoRecord> infos;
		if (spell_ids.empty())
		{
			return infos;
		}

		auto connection = connect(error);
		if (!connection)
		{
			return infos;
		}

		// Turtle/vmangos keep custom spells in spell_template; AzerothCore has no such table (spell data
		// lives in the client DBC there) -- return empty and let the caller degrade gracefully.
		if (!tableExists(connection.get(), "spell_template"))
		{
			return infos;
		}

		std::stringstream statement;
		statement << "SELECT entry, spellVisual1, spellIconId, school, name, description, "
		          << "effectBasePoints1, effectBasePoints2, effectBasePoints3, "
		          << "effectDieSides1, effectDieSides2, effectDieSides3, "
		          << "effectAmplitude1, effectAmplitude2, effectAmplitude3, durationIndex, "
		          << "effectChainTarget1, effectChainTarget2, effectChainTarget3, "
		          << "effectRadiusIndex1, effectRadiusIndex2, effectRadiusIndex3, "
		          << "effectMultipleValue1, effectMultipleValue2, effectMultipleValue3, "
		          << "maxAffectedTargets, stackAmount, procCharges, procChance, maxTargetLevel, "
		          << "manaCost, powerType, rangeIndex, castingTimeIndex "
		          << "FROM spell_template WHERE entry IN (";
		bool first = true;
		for (auto const id : spell_ids)
		{
			statement << (first ? "" : ",") << id;
			first = false;
		}
		statement << ")";

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return infos;
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return infos;
		}

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			SpellInfoRecord info;
			info.entry = parseUnsigned(row[0]);
			info.spell_visual = parseUnsigned(row[1]);
			info.icon_id = parseUnsigned(row[2]);
			info.school = parseUnsigned(row[3]);
			info.name = parseString(row[4]);
			info.description = parseString(row[5]);
			for (int i = 0; i < 3; ++i)
			{
				info.effect_base_points[i] = static_cast<std::int32_t>(std::strtol(row[6 + i] ? row[6 + i] : "0", nullptr, 10));
				info.effect_die_sides[i] = static_cast<std::int32_t>(std::strtol(row[9 + i] ? row[9 + i] : "0", nullptr, 10));
				info.effect_amplitude[i] = static_cast<std::int32_t>(std::strtol(row[12 + i] ? row[12 + i] : "0", nullptr, 10));
				info.effect_chain_target[i] = static_cast<std::int32_t>(std::strtol(row[16 + i] ? row[16 + i] : "0", nullptr, 10));
				info.effect_radius_index[i] = static_cast<std::int32_t>(std::strtol(row[19 + i] ? row[19 + i] : "0", nullptr, 10));
				info.effect_multiple_value[i] = parseFloat(row[22 + i]);
			}
			info.duration_index = parseUnsigned(row[15]);
			info.max_affected_targets = parseUnsigned(row[25]);
			info.stack_amount = parseUnsigned(row[26]);
			info.proc_charges = parseUnsigned(row[27]);
			info.proc_chance = parseUnsigned(row[28]);
			info.max_target_level = parseUnsigned(row[29]);
			info.mana_cost = parseUnsigned(row[30]);
			info.power_type = parseUnsigned(row[31]);
			info.range_index = parseUnsigned(row[32]);
			info.casting_time_index = parseUnsigned(row[33]);
			infos.emplace(info.entry, info);
		}

		mysql_free_result(result);
		return infos;
	}

	CreatureTemplateDetails getCreatureTemplateDetails(std::uint32_t entry, std::string* error)
	{
		CreatureTemplateDetails d;
		d.entry = entry;

		auto connection = connect(error);
		if (!connection)
		{
			return d;
		}

		if (!tableExists(connection.get(), "creature_template"))
		{
			if (error)
			{
				*error = "no creature_template table in this database";
			}
			return d;
		}

		// Resolve every field by NAME across the supported world schemas instead of assuming Turtle/vmangos.
		// The old code bailed out entirely when `level_min` was absent, so the Quick Facts panel showed
		// "creature_template schema not supported" on every 3.3.5a project: CMaNGOS calls it MinLevel and
		// AzerothCore minlevel. Each entry below emits EXACTLY ONE column so the fixed row[0..31] parsing
		// order stays valid; a field no schema provides falls back to a literal (0 / '').
		auto const col = [&](std::initializer_list<char const*> candidates, char const* fallback) -> std::string
		{
			for (auto const* candidate : candidates)
			{
				if (tableHasColumn(connection.get(), "creature_template", candidate))
				{
					return std::string("`") + candidate + "`";
				}
			}
			return fallback;
		};

		// Auras: Turtle keeps them inline on creature_template; CMaNGOS/AzerothCore put them on
		// creature_template_addon (a scalar subquery keeps this one column, so the indices don't shift).
		std::string auras_expr = "''";
		if (tableHasColumn(connection.get(), "creature_template", "auras"))
		{
			auras_expr = "COALESCE(`auras`, '')";
		}
		else if (tableHasColumn(connection.get(), "creature_template_addon", "auras"))
		{
			auras_expr = "COALESCE((SELECT cta.auras FROM creature_template_addon cta"
			             " WHERE cta.entry = creature_template.entry LIMIT 1), '')";
		}

		// AzerothCore has no inline display column -- it lives in creature_template_model.
		std::string display_expr = col({"display_id1", "DisplayId1"}, "");
		if (display_expr.empty())
		{
			display_expr = tableHasColumn(connection.get(), "creature_template_model", "CreatureDisplayID")
				? "COALESCE((SELECT ctm.CreatureDisplayID FROM creature_template_model ctm"
				  " WHERE ctm.CreatureID = creature_template.entry ORDER BY ctm.Idx LIMIT 1), 0)"
				: "0";
		}

		std::stringstream statement;
		statement << "SELECT "
		          << col({"name", "Name"}, "''") << ", "
		          << col({"subname", "SubName"}, "''") << ", "
		          << col({"level_min", "MinLevel", "minlevel"}, "0") << ", "
		          << col({"level_max", "MaxLevel", "maxlevel"}, "0") << ", "
		          << col({"rank", "Rank"}, "0") << ", "
		          << col({"faction", "Faction", "faction_A"}, "0") << ", "
		          << col({"npc_flags", "NpcFlags", "npcflag"}, "0") << ", "
		          << col({"health_min", "MinLevelHealth"}, "0") << ", "
		          << col({"health_max", "MaxLevelHealth"}, "0") << ", "
		          << col({"mana_min", "MinLevelMana"}, "0") << ", "
		          << col({"mana_max", "MaxLevelMana"}, "0") << ", "
		          << col({"gold_min", "MinLootGold", "mingold"}, "0") << ", "
		          << col({"gold_max", "MaxLootGold", "maxgold"}, "0") << ", "
		          << col({"dmg_min", "MinMeleeDmg"}, "0") << ", "
		          << col({"dmg_max", "MaxMeleeDmg"}, "0") << ", "
		          << col({"armor", "Armor"}, "0") << ", "
		          << col({"holy_res", "ResistanceHoly"}, "0") << ", "
		          << col({"fire_res", "ResistanceFire"}, "0") << ", "
		          << col({"nature_res", "ResistanceNature"}, "0") << ", "
		          << col({"frost_res", "ResistanceFrost"}, "0") << ", "
		          << col({"shadow_res", "ResistanceShadow"}, "0") << ", "
		          << col({"arcane_res", "ResistanceArcane"}, "0") << ", "
		          << display_expr << ", "
		          << col({"equipment_id", "EquipmentTemplateId"}, "0") << ", "
		          << col({"unit_class", "UnitClass"}, "0") << ", "
		          << col({"type", "CreatureType"}, "0") << ", "
		          << col({"spell_id1"}, "0") << ", "
		          << col({"spell_id2"}, "0") << ", "
		          << col({"spell_id3"}, "0") << ", "
		          << col({"spell_id4"}, "0") << ", "
		          << col({"spell_list_id", "SpellList"}, "0") << ", "
		          << auras_expr << " AS auras "
		          << "FROM creature_template WHERE "
		          << col({"entry", "Entry"}, "entry") << " = " << entry;

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return d;
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return d;
		}

		std::uint32_t spell_list_id = 0;
		if (MYSQL_ROW row = mysql_fetch_row(result))
		{
			d.name = parseString(row[0]);
			d.subname = parseString(row[1]);
			d.level_min = parseUnsigned(row[2]);
			d.level_max = parseUnsigned(row[3]);
			d.rank = parseUnsigned(row[4]);
			d.faction = parseUnsigned(row[5]);
			d.npc_flags = parseUnsigned(row[6]);
			d.health_min = parseUnsigned(row[7]);
			d.health_max = parseUnsigned(row[8]);
			d.mana_min = parseUnsigned(row[9]);
			d.mana_max = parseUnsigned(row[10]);
			d.gold_min = parseUnsigned(row[11]);
			d.gold_max = parseUnsigned(row[12]);
			d.dmg_min = parseFloat(row[13]);
			d.dmg_max = parseFloat(row[14]);
			d.armor = parseUnsigned(row[15]);
			d.holy_res = static_cast<std::int32_t>(std::strtol(row[16] ? row[16] : "0", nullptr, 10));
			d.fire_res = static_cast<std::int32_t>(std::strtol(row[17] ? row[17] : "0", nullptr, 10));
			d.nature_res = static_cast<std::int32_t>(std::strtol(row[18] ? row[18] : "0", nullptr, 10));
			d.frost_res = static_cast<std::int32_t>(std::strtol(row[19] ? row[19] : "0", nullptr, 10));
			d.shadow_res = static_cast<std::int32_t>(std::strtol(row[20] ? row[20] : "0", nullptr, 10));
			d.arcane_res = static_cast<std::int32_t>(std::strtol(row[21] ? row[21] : "0", nullptr, 10));
			d.display_id = parseUnsigned(row[22]);
			d.equipment_id = parseUnsigned(row[23]);
			d.unit_class = parseUnsigned(row[24]);
			d.type = parseUnsigned(row[25]);
			for (int i = 26; i <= 29; ++i)
			{
				auto const spell_id = parseUnsigned(row[i]);
				if (spell_id)
				{
					d.spells.push_back(spell_id);
				}
			}
			spell_list_id = parseUnsigned(row[30]);
			std::istringstream aura_tokens(parseString(row[31]));
			std::uint32_t aura_id = 0;
			while (aura_tokens >> aura_id)
			{
				if (aura_id)
				{
					d.auras.push_back(aura_id);
				}
			}
			d.ok = true;
		}
		mysql_free_result(result);

		// Scripted combat spells. Each world schema stores them somewhere different, and creature_template's
		// inline spell_id1..4 only exist on Turtle/vmangos -- which is why the 3.3.5a projects showed an EMPTY
		// "Spells:" row. Collect from whichever tables this database actually has:
		//   CMaNGOS  creature_template_spells (entry, spell1..spell10)   <- keyed by the CREATURE entry
		//   CMaNGOS  creature_spell_list (Id, SpellId)                   <- via creature_template.SpellList
		//   AC/Trin  creature_template_spell (CreatureID, Index, Spell)  <- one row per spell
		auto const add_spell = [&d](std::uint32_t spell_id)
		{
			if (spell_id && std::find(d.spells.begin(), d.spells.end(), spell_id) == d.spells.end())
			{
				d.spells.push_back(spell_id);
			}
		};

		auto const collect_spells = [&](std::string const& query)
		{
			if (mysql_query(connection.get(), query.c_str()) != 0)
			{
				return;
			}
			if (MYSQL_RES* result_set = mysql_store_result(connection.get()))
			{
				unsigned const columns = mysql_num_fields(result_set);
				while (MYSQL_ROW row = mysql_fetch_row(result_set))
				{
					for (unsigned i = 0; i < columns; ++i)
					{
						add_spell(parseUnsigned(row[i]));
					}
				}
				mysql_free_result(result_set);
			}
		};

		if (d.ok && tableHasColumn(connection.get(), "creature_template_spells", "spell1"))
		{
			std::stringstream q;
			q << "SELECT spell1, spell2, spell3, spell4, spell5, spell6, spell7, spell8, spell9, spell10 "
			  << "FROM creature_template_spells WHERE entry = " << entry;
			collect_spells(q.str());
		}

		if (d.ok && spell_list_id && tableHasColumn(connection.get(), "creature_spell_list", "SpellId"))
		{
			std::stringstream q;
			q << "SELECT SpellId FROM creature_spell_list WHERE Id = " << spell_list_id << " ORDER BY Position";
			collect_spells(q.str());
		}

		if (d.ok && tableHasColumn(connection.get(), "creature_template_spell", "Spell"))
		{
			std::stringstream q;
			q << "SELECT Spell FROM creature_template_spell WHERE CreatureID = " << entry << " ORDER BY `Index`";
			collect_spells(q.str());
		}

		// creature_spells list (Turtle): spell_list_id -> up to 8 scripted combat spells.
		if (d.ok && spell_list_id && tableExists(connection.get(), "creature_spells"))
		{
			std::stringstream spells_statement;
			spells_statement << "SELECT spellId_1, spellId_2, spellId_3, spellId_4, spellId_5, spellId_6, spellId_7, spellId_8 "
			                 << "FROM creature_spells WHERE entry = " << spell_list_id;
			if (mysql_query(connection.get(), spells_statement.str().c_str()) == 0)
			{
				if (MYSQL_RES* spells_result = mysql_store_result(connection.get()))
				{
					if (MYSQL_ROW row = mysql_fetch_row(spells_result))
					{
						for (int i = 0; i < 8; ++i)
						{
							auto const spell_id = parseUnsigned(row[i]);
							if (spell_id && std::find(d.spells.begin(), d.spells.end(), spell_id) == d.spells.end())
							{
								d.spells.push_back(spell_id);
							}
						}
					}
					mysql_free_result(spells_result);
				}
			}
		}

		return d;
	}

	std::map<std::uint32_t, float> getCreatureBoundingRadii(std::string* error)
	{
		std::map<std::uint32_t, float> radii;
		auto connection = connect(error);
		if (!connection)
		{
			return radii;
		}

		char const* query = nullptr;
		if (tableExists(connection.get(), "creature_display_info_addon"))
		{
			query = "SELECT display_id, bounding_radius FROM creature_display_info_addon";
		}
		else if (tableExists(connection.get(), "creature_model_info"))
		{
			query = "SELECT modelid, bounding_radius FROM creature_model_info";
		}
		else
		{
			return radii;
		}

		if (mysql_query(connection.get(), query) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return radii;
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			return radii;
		}
		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			radii.emplace(parseUnsigned(row[0]), parseFloat(row[1]));
		}
		mysql_free_result(result);
		return radii;
	}

	std::vector<GameObjectSpawnRecord> getGameObjectSpawns(std::size_t mapID, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		if (!tableExists(connection.get(), "gameobject")
		    || !tableExists(connection.get(), "gameobject_template")
		    || !tableHasColumn(connection.get(), "gameobject", "guid")
		    || !tableHasColumn(connection.get(), "gameobject", "id")
		    || !tableHasColumn(connection.get(), "gameobject", "map")
		    || !tableHasColumn(connection.get(), "gameobject_template", "entry"))
		{
			if (error)
			{
				*error = "gameobject/gameobject_template tables are missing required columns";
			}
			return {};
		}

		auto name_expr = firstColumnExpr(connection.get(),
		                                "gameobject_template",
		                                "gt",
		                                {"name", "Name"},
		                                "''");
		auto scale_expr = firstColumnExpr(connection.get(),
		                                 "gameobject_template",
		                                 "gt",
		                                 {"size", "scale"},
		                                 "1");
		auto display_expr = buildGameObjectDisplayExpr(connection.get());

		// Seasonal game-event membership (see getCreatureSpawns for the sign semantics).
		bool const has_event_gameobject = tableExists(connection.get(), "game_event_gameobject");
		std::string const event_col =
			  !has_event_gameobject ? ""
			: tableHasColumn(connection.get(), "game_event_gameobject", "event")      ? "event"
			: tableHasColumn(connection.get(), "game_event_gameobject", "eventEntry") ? "eventEntry"
			:                                                                            "";
		bool const join_event = has_event_gameobject && !event_col.empty();
		std::string const event_select = join_event ? "COALESCE(geg.gevent, 0) AS event" : "0 AS event";
		std::string const event_join = join_event
			? "LEFT JOIN (SELECT guid, MIN(" + event_col + ") AS gevent FROM game_event_gameobject GROUP BY guid) geg ON geg.guid = go.guid "
			: "";

		std::stringstream statement;
		statement
			<< "SELECT go.guid, go.id, go.map, go.position_x, go.position_y, go.position_z, go.orientation, "
			<< "COALESCE(" << name_expr << ", '') AS name, "
			<< "COALESCE(NULLIF(" << scale_expr << ", 0), 1) AS template_scale, "
			<< display_expr << ", "
			<< event_select << " "
			<< "FROM gameobject go "
			<< "INNER JOIN gameobject_template gt ON gt.entry = go.id "
			<< event_join
			<< "WHERE go.map = " << mapID << " "
			<< "ORDER BY name, go.guid";

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<GameObjectSpawnRecord> records;
		records.reserve(static_cast<std::size_t>(mysql_num_rows(result)));

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			GameObjectSpawnRecord record;
			record.guid = parseUnsigned(row[0]);
			record.entry = parseUnsigned(row[1]);
			record.map = parseUnsigned(row[2]);
			record.position_x = parseFloat(row[3]);
			record.position_y = parseFloat(row[4]);
			record.position_z = parseFloat(row[5]);
			record.orientation = parseFloat(row[6]);
			record.name = parseString(row[7]);
			record.template_scale = parseFloat(row[8]);
			record.display_id = parseUnsigned(row[9]);
			record.event = parseSigned(row[10]);
			if (record.template_scale <= 0.0f)
			{
				record.template_scale = 1.0f;
			}
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
  }

	std::vector<GameEventRecord> getGameEvents(std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		if (!tableExists(connection.get(), "game_event"))
		{
			return {};
		}

		// Mangos/Turtle: game_event(entry, description). AzerothCore/TrinityCore: game_event(eventEntry, description).
		std::string const entry_col =
			  tableHasColumn(connection.get(), "game_event", "entry")      ? "entry"
			: tableHasColumn(connection.get(), "game_event", "eventEntry") ? "eventEntry"
			:                                                                "entry";
		std::string const desc_col =
			  tableHasColumn(connection.get(), "game_event", "description") ? "description"
			: tableHasColumn(connection.get(), "game_event", "name")        ? "name"
			:                                                                 "''";

		std::stringstream statement;
		statement << "SELECT " << entry_col << ", COALESCE(" << desc_col << ", '') FROM game_event ORDER BY " << entry_col;

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<GameEventRecord> records;
		records.reserve(static_cast<std::size_t>(mysql_num_rows(result)));
		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			GameEventRecord record;
			record.entry = parseSigned(row[0]);
			record.description = parseString(row[1]);
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
	}

	std::vector<CreaturePatrolPoint> getCreaturePatrolPaths(std::size_t mapID, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		// Per-guid waypoints (vmangos/mangos-style `creature_movement`, keyed by creature.guid). Absent
		// table -> no patrol data, not an error.
		if (!tableExists(connection.get(), "creature_movement")
		    || !tableHasColumn(connection.get(), "creature_movement", "id")
		    || !tableHasColumn(connection.get(), "creature_movement", "point")
		    || !tableHasColumn(connection.get(), "creature_movement", "position_x"))
		{
			return {};
		}

		std::stringstream statement;
		statement
			<< "SELECT cm.id, cm.point, cm.position_x, cm.position_y, cm.position_z "
			<< "FROM creature_movement cm "
			<< "INNER JOIN creature c ON c.guid = cm.id "
			<< "WHERE c.map = " << mapID << " "
			<< "ORDER BY cm.id, cm.point";

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<CreaturePatrolPoint> points;
		points.reserve(static_cast<std::size_t>(mysql_num_rows(result)));

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			CreaturePatrolPoint point;
			point.guid = parseUnsigned(row[0]);
			point.point = parseUnsigned(row[1]);
			point.position_x = parseFloat(row[2]);
			point.position_y = parseFloat(row[3]);
			point.position_z = parseFloat(row[4]);
			points.push_back(point);
		}

		mysql_free_result(result);
		return points;
	}

	std::vector<CreatureSpawnRecord> searchCreatureSpawns(std::string const& searchTerm, std::size_t limit, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		if (searchTerm.empty())
		{
			return {};
		}

		bool needs_creature_addon_join = false;
		bool has_mount_display_col = false;
		bool has_template_scale_col = tableHasColumn(connection.get(), "creature_template", "scale");
		auto const equipment_schema = resolveCreatureEquipmentSchema(connection.get());
		auto display_expr = buildCreatureDisplayExpr(connection.get(), &needs_creature_addon_join, &has_mount_display_col);
		auto mount_expr = has_mount_display_col
			? "COALESCE(CASE WHEN ca.mount_display_id > 0 THEN ca.mount_display_id ELSE 0 END, 0) AS mount_display_id"
			: "0 AS mount_display_id";
		auto template_scale_expr = has_template_scale_col
			// Preserve a raw 0 (don't coerce to 1): creature_template.scale of 0 means "use the
			// CreatureDisplayInfo scale" (server ObjectMgr.cpp:1436). The caller applies that display-scale
			// fallback, which it can only do if it sees the real 0 rather than a coerced 1.0.
			? "COALESCE(ct.scale, 0) AS template_scale"
			: "0 AS template_scale";

		auto escaped_search = escapeString(connection.get(), searchTerm);
		std::stringstream statement;
		statement
			<< "SELECT c.guid, c.id, c.map, c.position_x, c.position_y, c.position_z, c.orientation, ct.name, "
			<< template_scale_expr << ", "
			<< display_expr << ", "
			<< mount_expr << ", "
			<< creatureEquipmentSelectExpr(equipment_schema) << " "
			<< "FROM creature c "
			<< "INNER JOIN creature_template ct ON ct.entry = c.id "
			<< (needs_creature_addon_join ? "LEFT JOIN creature_addon ca ON ca.guid = c.guid " : "")
			<< creatureEquipmentJoinExpr(equipment_schema)
			<< "WHERE ct.name LIKE '%" << escaped_search << "%' OR CAST(c.id AS CHAR) = '" << escaped_search << "' "
			<< "ORDER BY ct.name, c.map, c.guid "
			<< "LIMIT " << limit;

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<CreatureSpawnRecord> records;
		records.reserve(static_cast<std::size_t>(mysql_num_rows(result)));

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			CreatureSpawnRecord record;
			record.guid = parseUnsigned(row[0]);
			record.entry = parseUnsigned(row[1]);
			record.map = parseUnsigned(row[2]);
			record.position_x = parseFloat(row[3]);
			record.position_y = parseFloat(row[4]);
			record.position_z = parseFloat(row[5]);
			record.orientation = parseFloat(row[6]);
			record.name = parseString(row[7]);
			record.template_scale = parseFloat(row[8]);
			record.display_id = parseUnsigned(row[9]);
			record.mount_display_id = parseUnsigned(row[10]);
			record.mainhand_display_id = parseUnsigned(row[11]);
			record.offhand_display_id = parseUnsigned(row[12]);
			record.ranged_display_id = parseUnsigned(row[13]);
			record.mainhand_inventory_type = parseUnsigned(row[14]);
			record.offhand_inventory_type = parseUnsigned(row[15]);
			record.ranged_inventory_type = parseUnsigned(row[16]);
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
	}

	std::vector<CreatureTemplateRecord> getCreatureTemplates(std::size_t limit, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		if (!tableExists(connection.get(), "creature_template")
		    || !tableHasColumn(connection.get(), "creature_template", "entry")
		    || !tableHasColumn(connection.get(), "creature_template", "name"))
		{
			if (error)
			{
				*error = "creature_template table is missing required entry/name columns";
			}
			return {};
		}

		auto faction_expr = firstTemplateColumnExpr(connection.get(),
		                                           {"faction", "faction_A", "faction_a", "factionAlliance", "factionHorde"},
		                                           "0");
		auto creature_type_expr = firstTemplateColumnExpr(connection.get(), {"type", "creature_type", "creatureType"}, "0");
		auto rank_expr = firstTemplateColumnExpr(connection.get(), {"rank"}, "0");
		auto npc_flags_expr = firstTemplateColumnExpr(connection.get(), {"npcflag", "npc_flags", "npcFlags"}, "0");
		auto type_flags_expr = firstTemplateColumnExpr(connection.get(), {"type_flags", "typeFlags"}, "0");
		auto flags_extra_expr = firstTemplateColumnExpr(connection.get(), {"flags_extra", "flagsExtra"}, "0");
		auto scale_expr = firstTemplateColumnExpr(connection.get(), {"scale"}, "1");
		auto display_expr = buildCreatureTemplateDisplayExpr(connection.get());

		std::stringstream statement;
		statement
			<< "SELECT ct.entry, ct.name, "
			<< "COALESCE(" << faction_expr << ", 0) AS faction, "
			<< "COALESCE(" << creature_type_expr << ", 0) AS creature_type, "
			<< "COALESCE(" << rank_expr << ", 0) AS rank, "
			<< "COALESCE(" << npc_flags_expr << ", 0) AS npc_flags, "
			<< "COALESCE(" << type_flags_expr << ", 0) AS type_flags, "
			<< "COALESCE(" << flags_extra_expr << ", 0) AS flags_extra, "
			<< display_expr << ", "
			// Raw 0 preserved so the caller can apply the CreatureDisplayInfo scale fallback (see getCreatureSpawns).
			<< "COALESCE(" << scale_expr << ", 0) AS template_scale "
			<< "FROM creature_template ct "
			<< "ORDER BY ct.entry "
			<< "LIMIT " << limit;

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<CreatureTemplateRecord> records;
		records.reserve(static_cast<std::size_t>(mysql_num_rows(result)));

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			CreatureTemplateRecord record;
			record.entry = parseUnsigned(row[0]);
			record.name = parseString(row[1]);
			record.faction = parseUnsigned(row[2]);
			record.creature_type = parseUnsigned(row[3]);
			record.rank = parseUnsigned(row[4]);
			record.npc_flags = parseUnsigned(row[5]);
			record.type_flags = parseUnsigned(row[6]);
			record.flags_extra = parseUnsigned(row[7]);
			record.display_id = parseUnsigned(row[8]);
			// Keep a raw 0 here; the picker resolves the CreatureDisplayInfo scale fallback for scale-0 templates.
			record.template_scale = parseFloat(row[9]);
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
	}

	std::vector<GameObjectTemplateRecord> getGameObjectTemplates(std::size_t limit, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return {};
		}

		if (!tableExists(connection.get(), "gameobject_template")
		    || !tableHasColumn(connection.get(), "gameobject_template", "entry"))
		{
			if (error)
			{
				*error = "gameobject_template table is missing required entry column";
			}
			return {};
		}

		auto name_expr = firstColumnExpr(connection.get(),
		                                "gameobject_template",
		                                "gt",
		                                {"name", "Name"},
		                                "''");
		auto display_expr = firstColumnExpr(connection.get(),
		                                   "gameobject_template",
		                                   "gt",
		                                   {"displayId", "display_id", "DisplayId"},
		                                   "0");
		auto type_expr = firstColumnExpr(connection.get(),
		                                "gameobject_template",
		                                "gt",
		                                {"type", "Type"},
		                                "0");
		auto scale_expr = firstColumnExpr(connection.get(),
		                                 "gameobject_template",
		                                 "gt",
		                                 {"size", "scale"},
		                                 "1");

		std::stringstream statement;
		statement
			<< "SELECT gt.entry, "
			<< "COALESCE(" << name_expr << ", '') AS name, "
			<< "COALESCE(" << display_expr << ", 0) AS display_id, "
			<< "COALESCE(" << type_expr << ", 0) AS type, "
			<< "COALESCE(NULLIF(" << scale_expr << ", 0), 1) AS template_scale "
			<< "FROM gameobject_template gt "
			<< "ORDER BY name, gt.entry "
			<< "LIMIT " << limit;

		if (mysql_query(connection.get(), statement.str().c_str()) != 0)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		MYSQL_RES* result = mysql_store_result(connection.get());
		if (!result)
		{
			if (error)
			{
				*error = mysql_error(connection.get());
			}
			return {};
		}

		std::vector<GameObjectTemplateRecord> records;
		records.reserve(static_cast<std::size_t>(mysql_num_rows(result)));

		while (MYSQL_ROW row = mysql_fetch_row(result))
		{
			GameObjectTemplateRecord record;
			record.entry = parseUnsigned(row[0]);
			record.name = parseString(row[1]);
			record.display_id = parseUnsigned(row[2]);
			record.type = parseUnsigned(row[3]);
			record.template_scale = parseFloat(row[4]);
			if (record.template_scale <= 0.0f)
			{
				record.template_scale = 1.0f;
			}
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
	}

	bool updateCreatureSpawn(std::uint32_t guid, float position_x, float position_y, float position_z, float orientation, std::string* error)
	{
		auto connection = connect(error);
		if (!connection)
		{
			return false;
		}

		std::stringstream statement;
		statement
			<< "UPDATE creature SET "
			<< "position_x = " << position_x << ", "
			<< "position_y = " << position_y << ", "
			<< "position_z = " << position_z << ", "
			<< "orientation = " << orientation << " "
			<< "WHERE guid = " << guid;

		if (!executeStatement(connection.get(), statement.str(), error))
		{
			return false;
		}

		return mysql_affected_rows(connection.get()) >= 0;
	}
}
