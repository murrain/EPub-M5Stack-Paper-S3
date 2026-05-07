// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once
#include "global.hpp"

#include "pugixml.hpp"

#include "models/css.hpp"
#include "models/book_params.hpp"
#include "viewers/page.hpp"
#include "models/image.hpp"

#include <list>
#include <forward_list>
#include <map>
#include <mutex>

class EPub
{
  public:
    enum class       MediaType : uint8_t { XML, JPEG, PNG, GIF, BMP };
    enum class ObfuscationType : uint8_t { NONE, ADOBE, IDPF, UNKNOWN };
    typedef std::list<CSS *> CSSList;
    struct ItemInfo {
      std::string        file_path;
      int16_t            itemref_index;
      pugi::xml_document xml_doc;
      CSSList            css_cache;   ///< style attributes part of the current processed item are kept here. They will be destroyed when the item is no longer required.
      CSSList            css_list;    ///< List of css sources for the current item file shown. Those are indexes inside css_cache.
      CSS *              css;         ///< Ghost CSS created through merging css suites from css_list and css_cache.
      char *             data;
      MediaType          media_type;
    };

    // This struct contains the current parameters that influence
    // the rendering of e-book pages. Its content is constructed from
    // both the e-book's specific parameters and default configuration options.
    #pragma pack(push, 1)
    struct BookFormatParams {
      int8_t ident;        ///< Device identity (screen.hpp IDENT constant)
      int8_t orientation;  ///< Config option only
      int8_t show_title;
      int8_t show_images;      
      int8_t font_size;        
      int8_t use_fonts_in_book;
      int8_t font;             
    };
    #pragma pack(pop)

    typedef uint8_t BinUUID[16];
    typedef uint8_t ShaUUID[20];
    
  private:
    static constexpr char const * TAG = "EPub";

    std::recursive_timed_mutex mutex;

    pugi::xml_document opf;    ///< The OPF document description.
    pugi::xml_document encryption;
    pugi::xml_node     current_itemref;

    BinUUID            bin_uuid;
    ShaUUID            sha_uuid;

    char *             opf_data;
    char *             encryption_data;
    std::string        current_filename;
    std::string        opf_base_path;

    ItemInfo           current_item_info;
    BookParams       * book_params;
    BookFormatParams   book_format_params;

    CSSList            css_cache;             ///< All css files in the ebook are maintained here.
  
    bool               file_is_open;
    bool               encryption_present;
    bool               fonts_size_too_large;
    int32_t            fonts_size;

    const char *             get_meta(const std::string    & name         );
    bool                      get_opf(std::string          & filename     );
    bool               check_mimetype();
    bool             get_opf_filename(std::string          & filename     );
    void      retrieve_fonts_from_css(CSS                  & css          );
    bool           get_encryption_xml();
    void                         sha1(const std::string    & data         );

  public:
    EPub();
   ~EPub();

    void                 retrieve_css(ItemInfo             & item         );
    void                   load_fonts();

    /**
     * @brief Refresh book_format_params from book_params + config.
     *
     * Recomputes the cached `book_format_params` struct by reading
     * the per-book `book_params` (FONT, FONT_SIZE, etc.) with
     * fallback to the global `config` (DEFAULT_FONT, etc.) for
     * any field set to the -1 sentinel.
     *
     * @param invalidate_dependent_caches Default true. After the
     *        recompute, drops PageCache entries and the warm-wake
     *        snapshot — both reflect the OLD layout and would
     *        repaint stale content if read after a format change.
     *        Pass false ONLY from EPub::open_file, which calls
     *        this on a fresh book where the cache is already
     *        empty (close_file just ran) AND we want to PRESERVE
     *        any warm-wake snapshot that may match the just-opened
     *        book id (open_file is the entry point for warm wake).
     *
     * Previously the invalidation lived as scattered calls in
     * book_param_controller (always remembered) and option_controller
     * (forgotten — see audit 00-summary.md "🟡 option_controller
     * doesn't invalidate page_cache after format-param changes").
     * Centralizing here fixes the bug AND removes 4 sites of
     * duplication.
     */
    void update_book_format_params(bool invalidate_dependent_caches = true);

    void              clear_item_data(ItemInfo             & item         );
    void                  open_params(const std::string    & epub_filename);
    bool                    open_file(const std::string    & epub_filename);
    bool                   close_file();

    /**
     * @brief Quiesce the live book session before tearing down state.
     *
     * Stops the pre-paint pthread and waits for the page-locs
     * retriever to confirm STOPPED. Returns true if BOTH are
     * provably idle — at that point it's safe to free EPUB state
     * (close_file, fonts.clear, font swap), pause for sleep, or
     * mutate format params. Returns false if page_locs.stop_
     * document timed out, meaning the retriever is still alive
     * and may still hold references into item_info / opf / css /
     * unzip / Font*. Callers MUST honor a false return by
     * skipping any teardown that would free those — close_file,
     * font cache mutation, page_locs.clear, or open_file (which
     * close-then-opens internally). Without that gate, the
     * retriever's next dereference page-faults (LoadProhibited
     * UAF observed in the menu→book→menu→book→menu sequence).
     *
     * Lock-order: page_cache (pre-paint) MUST stop first; it
     * holds book_viewer.get_mutex during render and dereferences
     * page_locs.item_info, so freeing those before the pre-paint
     * task is confirmed idle would UAF mid-render.
     */
    static bool quiesce_book_session();
    Image *                 get_image(std::string          & fname,
                                      bool                   load         );
    char*               retrieve_file(const char           * fname, 
                                      uint32_t             & size         );
    bool                     get_item(pugi::xml_node         itemref, 
                                      ItemInfo             & item         );
    bool            get_item_at_index(int16_t                itemref_index);
    bool            get_item_at_index(int16_t                itemref_index,
                                      ItemInfo             & item         );
    std::string get_unique_identifier();
    bool                     get_keys();
    std::string       filename_locate(const char           * fname        );
    int16_t            get_item_count();
    // Declaration moved to public section above (lines 99-127) and
    // gained a bool parameter for cache invalidation. Old void
    // signature deleted.
    ObfuscationType get_file_obfuscation(const char        * filename     );
    void                      decrypt(void                 * buffer, 
                                      const uint32_t         size,
                                      ObfuscationType        obf_type     );
    bool                    load_font(const std::string      filename, 
                                      const std::string      font_family, 
                                      const Fonts::FaceStyle style        );
    /**
     * @brief Retrieve cover's filename
     *
     * Look inside the opf file to grab the cover filename. First search in the
     * metadata. If not found, search in the manifest for an entry with type
     * cover-image
     *
     * @return char * filename, or nullptr if not found
     */
    const char* get_cover_filename();

    inline const CSSList &                   get_css_cache() const { return css_cache;                       }
    inline CSS *                      get_current_item_css() const { return current_item_info.css;           }
    inline const ItemInfo &          get_current_item_info() const { return current_item_info; }
    inline const std::string &  get_current_item_file_path() const { return current_item_info.file_path;     }
    inline int16_t                       get_itemref_index() const { return current_item_info.itemref_index; }
    inline const char *                          get_title()       { return get_meta("dc:title");            }
    inline const char *                         get_author()       { return get_meta("dc:creator");          }
    inline const char *                    get_description()       { return get_meta("dc:description");      }
    inline const pugi::xml_document &     get_current_item() const { return current_item_info.xml_doc;       }
    inline std::string                get_current_filename()       { return current_filename;                }
    inline bool                              is_file_open() const { return file_is_open;                    }
    inline bool                          filename_is_empty()       { return current_filename.empty();        }
    inline BookParams *                    get_book_params()       { return book_params;                     }
    inline BookFormatParams *       get_book_format_params()       { return &book_format_params;             }
    inline const std::string &           get_opf_base_path() const { return opf_base_path;                   }
    inline const pugi::xml_document &              get_opf()       { return opf;                             }
    inline bool                      encryption_is_present() const { return encryption_present;              }
    inline const BinUUID &                    get_bin_uuid() const { return bin_uuid;                        }

    // Predicate-chain accessors for the three top-level OPF
    // sections. Replaces 12+ scattered `opf.find_child(package_pred).
    // find_child(<section>_pred)` chains across epub.cpp and toc.cpp.
    // Each returns an empty xml_node if the section is missing — the
    // pugixml convention every existing call site already handles.
    // Helpers live on EPub rather than as free functions so callers
    // that hold a pointer to the OPF (TOC has `opf = &epub.get_opf()`)
    // can switch to `epub.manifest_node()` etc. without restructuring.
    pugi::xml_node    package_node();
    pugi::xml_node   manifest_node();
    pugi::xml_node      spine_node();
    pugi::xml_node   metadata_node();
  };

#if __EPUB__
  EPub epub;
#else
  extern EPub epub;
#endif
