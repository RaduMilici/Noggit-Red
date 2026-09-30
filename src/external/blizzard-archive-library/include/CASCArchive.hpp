#ifndef BLIZZARDARCHIVE_CASCARCHIVE_HPP
#define BLIZZARDARCHIVE_CASCARCHIVE_HPP

#include <BaseArchive.hpp>

#include <cstdint>

namespace BlizzardArchive::Listfile
{
  class Listfile;
}

namespace BlizzardArchive::Archive
{

  class CASCArchive : public BaseArchive
  {
  public:
    // product: the .build.info "Product" code selecting the build inside a multi-product store
    // ("wow", "wow_classic_era", "wow_anniversary", ...). CascLib needs it verbatim.
    CASCArchive(std::string const& path, std::string const& cache_path, std::string const& product, Locale locale, OpenMode open_mode, Listfile::Listfile* listfile);
    ~CASCArchive() override;

    // Which duplicate root record wins for a FileDataId whose content is local under more than one
    // record: the one whose content flags equal `value` under `mask` (mask 0 = first record, as upstream
    // CascLib). Must be called BEFORE the storage is opened; the root manifest is resolved once then.
    // Modern WoW roots use bit 0x1 for the high-resolution texture variant (docs/client_re/42 sec 18).
    static void setPreferredContentFlags(std::uint32_t mask, std::uint32_t value);

    [[nodiscard]]
    bool openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const override;

    bool readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const override;
    bool closeFile(HANDLE file_handle) const override;

    [[nodiscard]]
    std::uint64_t getFileSize(HANDLE file_handle) const override;

    [[nodiscard]]
    bool exists(Listfile::FileKey const& file_key, Locale locale) const override;

  private:
    HANDLE _handle = nullptr;
    std::string _product; // kept alive for CascOpenStorageEx (szCodeName points into it)
  };

}

#endif //BLIZZARDARCHIVE_CASCARCHIVE_HPP