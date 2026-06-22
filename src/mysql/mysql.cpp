// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql/mysql.h>
#include <mysql.h>

#include <QtCore/QSettings>
#include <QMessageBox>

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
		QSettings settings;

		ConnectionDetails details;
		details.host = settings.value("project/mysql/server", "127.0.0.1").toString().toStdString();
		details.user = settings.value("project/mysql/user", "root").toString().toStdString();
		details.password = settings.value("project/mysql/pwd", "mangos").toString().toStdString();
		details.schema = settings.value("project/mysql/db", "tw_world").toString().toStdString();
		details.port = settings.value("project/mysql/port", 3306).toUInt();

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

	bool hasCreatureEquipmentSchema(MYSQL* connection)
	{
		return tableHasColumn(connection, "creature_template", "equipment_id")
		    && tableExists(connection, "creature_equip_template")
		    && tableHasColumn(connection, "creature_equip_template", "entry")
		    && tableHasColumn(connection, "creature_equip_template", "equipentry1")
		    && tableHasColumn(connection, "creature_equip_template", "equipentry2")
		    && tableHasColumn(connection, "creature_equip_template", "equipentry3")
		    && tableExists(connection, "item_template")
		    && tableHasColumn(connection, "item_template", "entry")
		    && tableHasColumn(connection, "item_template", "display_id")
		    && tableHasColumn(connection, "item_template", "inventory_type");
	}

	std::string creatureEquipmentSelectExpr(bool has_equipment_schema)
	{
		if (!has_equipment_schema)
		{
			return "0 AS mainhand_display_id, 0 AS offhand_display_id, 0 AS ranged_display_id, "
			       "0 AS mainhand_inventory_type, 0 AS offhand_inventory_type, 0 AS ranged_inventory_type";
		}

		return "COALESCE(it1.display_id, 0) AS mainhand_display_id, "
		       "COALESCE(it2.display_id, 0) AS offhand_display_id, "
		       "COALESCE(it3.display_id, 0) AS ranged_display_id, "
		       "COALESCE(it1.inventory_type, 0) AS mainhand_inventory_type, "
		       "COALESCE(it2.inventory_type, 0) AS offhand_inventory_type, "
		       "COALESCE(it3.inventory_type, 0) AS ranged_inventory_type";
	}

	std::string creatureEquipmentJoinExpr(bool has_equipment_schema)
	{
		if (!has_equipment_schema)
		{
			return {};
		}

		return "LEFT JOIN creature_equip_template cet ON cet.entry = ct.equipment_id "
		       "LEFT JOIN item_template it1 ON it1.entry = cet.equipentry1 "
		       "LEFT JOIN item_template it2 ON it2.entry = cet.equipentry2 "
		       "LEFT JOIN item_template it3 ON it3.entry = cet.equipentry3 ";
	}

	std::string buildCreatureDisplayExpr(MYSQL* connection, bool* needs_creature_addon_join, bool* has_mount_display_col)
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

		bool const has_template_display_ids = tableHasColumn(connection, "creature_template", "display_id1")
		                                && tableHasColumn(connection, "creature_template", "display_id2")
		                                && tableHasColumn(connection, "creature_template", "display_id3")
		                                && tableHasColumn(connection, "creature_template", "display_id4");
		if (has_template_display_ids)
		{
			std::string const display_count = "((ct.display_id1 > 0) + (ct.display_id2 > 0) + (ct.display_id3 > 0) + (ct.display_id4 > 0))";
			std::string const first_display = "COALESCE(NULLIF(ct.display_id1, 0), NULLIF(ct.display_id2, 0), NULLIF(ct.display_id3, 0), NULLIF(ct.display_id4, 0))";
			std::string const second_display = "CASE "
				"WHEN (ct.display_id1 > 0) + (ct.display_id2 > 0) = 2 THEN ct.display_id2 "
				"WHEN (ct.display_id1 > 0) + (ct.display_id2 > 0) + (ct.display_id3 > 0) = 2 THEN ct.display_id3 "
				"WHEN (ct.display_id1 > 0) + (ct.display_id2 > 0) + (ct.display_id3 > 0) + (ct.display_id4 > 0) = 2 THEN ct.display_id4 END";
			std::string const third_display = "CASE "
				"WHEN (ct.display_id1 > 0) + (ct.display_id2 > 0) + (ct.display_id3 > 0) = 3 THEN ct.display_id3 "
				"WHEN (ct.display_id1 > 0) + (ct.display_id2 > 0) + (ct.display_id3 > 0) + (ct.display_id4 > 0) = 3 THEN ct.display_id4 END";
			std::string const fourth_display = "CASE WHEN " + display_count + " = 4 THEN ct.display_id4 END";

			std::stringstream template_display_expr;
			template_display_expr << "CASE WHEN " << display_count << " > 0 THEN CASE 1 + MOD(c.guid, GREATEST(" << display_count << ", 1)) "
			                      << "WHEN 1 THEN " << first_display << " "
			                      << "WHEN 2 THEN " << second_display << " "
			                      << "WHEN 3 THEN " << third_display << " "
			                      << "WHEN 4 THEN " << fourth_display << " END END";
			parts.emplace_back(template_display_expr.str());
		}

		parts.insert(parts.end(), ambiguous_spawn_parts.begin(), ambiguous_spawn_parts.end());

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
		bool has_mount_display_col = false;
		bool has_template_scale_col = tableHasColumn(connection.get(), "creature_template", "scale");
		bool const has_equipment_schema = hasCreatureEquipmentSchema(connection.get());
		auto display_expr = buildCreatureDisplayExpr(connection.get(), &needs_creature_addon_join, &has_mount_display_col);
		auto mount_expr = has_mount_display_col
			? "COALESCE(CASE WHEN ca.mount_display_id > 0 THEN ca.mount_display_id ELSE 0 END, 0) AS mount_display_id"
			: "0 AS mount_display_id";
		auto template_scale_expr = has_template_scale_col
			? "COALESCE(NULLIF(ct.scale, 0), 1) AS template_scale"
			: "1 AS template_scale";

		std::stringstream statement;
		statement
			<< "SELECT c.guid, c.id, c.map, c.position_x, c.position_y, c.position_z, c.orientation, ct.name, "
			<< template_scale_expr << ", "
			<< display_expr << ", "
			<< mount_expr << ", "
			<< creatureEquipmentSelectExpr(has_equipment_schema) << " "
			<< "FROM creature c "
			<< "INNER JOIN creature_template ct ON ct.entry = c.id "
			<< (needs_creature_addon_join ? "LEFT JOIN creature_addon ca ON ca.guid = c.guid " : "")
			<< creatureEquipmentJoinExpr(has_equipment_schema)
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
			records.push_back(record);
		}

		mysql_free_result(result);
		return records;
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
		bool const has_equipment_schema = hasCreatureEquipmentSchema(connection.get());
		auto display_expr = buildCreatureDisplayExpr(connection.get(), &needs_creature_addon_join, &has_mount_display_col);
		auto mount_expr = has_mount_display_col
			? "COALESCE(CASE WHEN ca.mount_display_id > 0 THEN ca.mount_display_id ELSE 0 END, 0) AS mount_display_id"
			: "0 AS mount_display_id";
		auto template_scale_expr = has_template_scale_col
			? "COALESCE(NULLIF(ct.scale, 0), 1) AS template_scale"
			: "1 AS template_scale";

		auto escaped_search = escapeString(connection.get(), searchTerm);
		std::stringstream statement;
		statement
			<< "SELECT c.guid, c.id, c.map, c.position_x, c.position_y, c.position_z, c.orientation, ct.name, "
			<< template_scale_expr << ", "
			<< display_expr << ", "
			<< mount_expr << ", "
			<< creatureEquipmentSelectExpr(has_equipment_schema) << " "
			<< "FROM creature c "
			<< "INNER JOIN creature_template ct ON ct.entry = c.id "
			<< (needs_creature_addon_join ? "LEFT JOIN creature_addon ca ON ca.guid = c.guid " : "")
			<< creatureEquipmentJoinExpr(has_equipment_schema)
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
			<< "COALESCE(NULLIF(" << scale_expr << ", 0), 1) AS template_scale "
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
			record.template_scale = parseFloat(row[9]);
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
