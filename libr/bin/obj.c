/* radare2 - LGPL - Copyright 2009-2019 - pancake, nibble, dso */

#include <r_bin.h>
#include <r_util.h>
#include "i/private.h"

static void mem_free(void *data) {
	RBinMem *mem = (RBinMem *)data;
	if (mem && mem->mirrors) {
		mem->mirrors->free = mem_free;
		r_list_free (mem->mirrors);
		mem->mirrors = NULL;
	}
	free (mem);
}

static int reloc_cmp(const void *a, const RBNode *b) {
	const RBinReloc *ar = (const RBinReloc *)a;
	const RBinReloc *br = container_of (b, const RBinReloc, vrb);
	if (ar->vaddr > br->vaddr) {
		return 1;
	}
	if (ar->vaddr < br->vaddr) {
		return -1;
	}
	return 0;
}

static void reloc_free(RBNode *rbn) {
	free (container_of (rbn, RBinReloc, vrb));
}

static void object_delete_items(RBinObject *o) {
	ut32 i = 0;
	r_return_if_fail (o);
	sdb_free (o->addr2klassmethod);
	r_list_free (o->entries);
	r_list_free (o->fields);
	r_list_free (o->imports);
	r_list_free (o->libs);
	r_rbtree_free (o->relocs, reloc_free);
	r_list_free (o->sections);
	r_list_free (o->strings);
	ht_up_free (o->strings_db);
	r_list_free (o->symbols);
	r_list_free (o->classes);
	ht_pp_free (o->classes_ht);
	r_list_free (o->lines);
	sdb_free (o->kv);
	if (o->mem) {
		o->mem->free = mem_free;
	}
	r_list_free (o->mem);
	for (i = 0; i < R_BIN_SYM_LAST; i++) {
		free (o->binsym[i]);
	}
}

R_IPI void r_bin_object_free(void /*RBinObject*/ *o_) {
	RBinObject *o = o_;
	if (!o) {
		return;
	}
	free (o->regstate);
	r_bin_info_free (o->info);
	object_delete_items (o);
	free (o);
}

static char *swiftField(const char *dn, const char *cn) {
	if (!dn || !cn) {
		return NULL;
	}

	char *p = strstr (dn, ".getter_");
	if (!p) {
		p = strstr (dn, ".setter_");
		if (!p) {
			p = strstr (dn, ".method_");
		}
	}
	if (p) {
		char *q = strstr (dn, cn);
		if (q && q[strlen (cn)] == '.') {
			q = strdup (q + strlen (cn) + 1);
			char *r = strchr (q, '.');
			if (r) {
				*r = 0;
			}
			return q;
		}
	}
	return NULL;
}

static RList *classes_from_symbols(RBinFile *bf) {
	RBinObject *o = bf->o;
	RBinSymbol *sym;
	RListIter *iter;
	RList *symbols = o->symbols;
	RList *classes = o->classes;
	if (!classes) {
		classes = r_list_newf ((RListFree)r_bin_class_free);
	}
	r_list_foreach (symbols, iter, sym) {
		if (sym->name[0] != '_') {
			continue;
		}
		const char *cn = sym->classname;
		if (cn) {
			RBinClass *c = r_bin_class_new (bf, sym->classname, NULL, 0);
			if (!c) {
				continue;
			}
			// swift specific
			char *dn = sym->dname;
			char *fn = swiftField (dn, cn);
			if (fn) {
				// eprintf ("FIELD %s  %s\n", cn, fn);
				RBinField *f = r_bin_field_new (sym->paddr, sym->vaddr, sym->size, fn, NULL, NULL);
				r_list_append (c->fields, f);
				free (fn);
			} else {
				char *mn = strstr (dn, "..");
				if (!mn) {
					mn = strstr (dn, cn);
					if (mn && mn[strlen (cn)] == '.') {
						mn += strlen (cn) + 1;
						// eprintf ("METHOD %s  %s\n", sym->classname, mn);
						r_list_append (c->methods, sym);
					}
				}
			}
		}
	}
	if (r_list_empty (classes)) {
		r_list_free (classes);
		return NULL;
	}
	return classes;
}

// TODO: kill offset and sz, because those should be infered from binfile->buf
R_IPI RBinObject *r_bin_object_new(RBinFile *bf, RBinPlugin *plugin, ut64 baseaddr, ut64 loadaddr, ut64 offset, ut64 sz) {
	r_return_val_if_fail (bf && plugin, NULL);
	ut64 bytes_sz = r_buf_size (bf->buf);
	Sdb *sdb = bf->sdb;
	RBinObject *o = R_NEW0 (RBinObject);
	if (!o) {
		return NULL;
	}
	o->obj_size = (bytes_sz >= sz + offset)? sz: 0;
	o->boffset = offset;
	o->strings_db = ht_up_new0 ();
	o->regstate = NULL;
	if (!r_id_pool_grab_id (bf->rbin->ids->pool, &o->id)) {
		free (o);
		eprintf ("Cannot grab an id\n");
		return NULL;
	}
	o->kv = sdb_new0 ();
	o->baddr = baseaddr;
	o->baddr_shift = 0;
	o->plugin = plugin;
	o->loadaddr = loadaddr != UT64_MAX ? loadaddr : 0;

	if (plugin && plugin->load_buffer) {
		if (!plugin->load_buffer (bf, &o->bin_obj, bf->buf, loadaddr, sdb)) {
			if (bf->rbin->verbose) {
				eprintf ("Error in r_bin_object_new: load_buffer failed for %s plugin\n", plugin->name);
			}
			sdb_free (o->kv);
			free (o);
			return NULL;
		}
	} else {
		R_LOG_WARN ("Plugin %s should implement load_buffer method.\n", plugin->name);
		sdb_free (o->kv);
		free (o);
		return NULL;
	}

	// XXX - object size cant be set here and needs to be set where where
	// the object is created from. The reason for this is to prevent
	// mis-reporting when the file is loaded from impartial bytes or is
	// extracted from a set of bytes in the file
	r_bin_object_set_items (bf, o);
	r_bin_file_set_cur_binfile_obj (bf->rbin, bf, o);

	bf->sdb_info = o->kv;
	sdb = bf->rbin->sdb;
	if (sdb) {
		Sdb *bdb = bf->sdb; // sdb_new0 ();
		sdb_ns_set (bdb, "info", o->kv);
		sdb_ns_set (bdb, "addrinfo", bf->sdb_addrinfo);
		o->kv = bdb;
		// bf->sdb = o->kv;
		// bf->sdb_info = o->kv;
		// sdb_ns_set (bf->sdb, "info", o->kv);
		//sdb_ns (sdb, sdb_fmt ("fd.%d", bf->fd), 1);
		sdb_set (bf->sdb, "archs", "0:0:x86:32", 0); // x86??
		/* NOTE */
		/* Those refs++ are necessary because sdb_ns() doesnt rerefs all
		 * sub-namespaces */
		/* And if any namespace is referenced backwards it gets
		 * double-freed */
		// bf->sdb_info = sdb_ns (bf->sdb, "info", 1);
	//	bf->sdb_addrinfo = sdb_ns (bf->sdb, "addrinfo", 1);
	//	bf->sdb_addrinfo->refs++;
		sdb_ns_set (sdb, "cur", bdb); // bf->sdb);
		const char *fdns = sdb_fmt ("fd.%d", bf->fd);
		sdb_ns_set (sdb, fdns, bdb); // bf->sdb);
		bf->sdb->refs++;
	}
	return o;
}

static void filter_classes(RBinFile *bf, RList *list) {
	Sdb *db = sdb_new0 ();
	HtPP *ht = ht_pp_new0 ();
	RListIter *iter, *iter2;
	RBinClass *cls;
	RBinSymbol *sym;
	r_list_foreach (list, iter, cls) {
		if (!cls->name) {
			continue;
		}
		int namepad_len = strlen (cls->name) + 32;
		char *namepad = malloc (namepad_len + 1);
		if (namepad) {
			char *p;
			strcpy (namepad, cls->name);
			p = r_bin_filter_name (bf, db, cls->index, namepad);
			if (p) {
				namepad = p;
			}
			free (cls->name);
			cls->name = namepad;
			r_list_foreach (cls->methods, iter2, sym) {
				if (sym->name) {
					r_bin_filter_sym (bf, ht, sym->vaddr, sym);
				}
			}
		} else {
			eprintf ("Cannot alloc %d byte(s)\n", namepad_len);
		}
	}
	sdb_free (db);
	ht_pp_free (ht);
}

static RBNode *list2rbtree(RList *relocs) {
	RListIter *it;
	RBinReloc *reloc;
	RBNode *res = NULL;

	r_list_foreach (relocs, it, reloc) {
		r_rbtree_insert (&res, reloc, &reloc->vrb, reloc_cmp);
	}
	return res;
}

R_API int r_bin_object_set_items(RBinFile *binfile, RBinObject *o) {
	int i;
	bool isSwift = false;

	r_return_val_if_fail (binfile && o && o->plugin, false);

	RBin *bin = binfile->rbin;
	RBinObject *old_o = binfile->o;
	RBinPlugin *cp = o->plugin;
	int minlen = (binfile->rbin->minstrlen > 0) ? binfile->rbin->minstrlen : cp->minstrlen;
	binfile->o = o;

	if (cp->file_type) {
		int type = cp->file_type (binfile);
		if (type == R_BIN_TYPE_CORE) {
			if (cp->regstate) {
				o->regstate = cp->regstate (binfile);
			}
			if (cp->maps) {
				o->maps = cp->maps (binfile);
			}
		}
	}

	if (cp->baddr) {
		ut64 old_baddr = o->baddr;
		o->baddr = cp->baddr (binfile);
		r_bin_object_set_baddr (o, old_baddr);
	}
	if (cp->boffset) {
		o->boffset = cp->boffset (binfile);
	}
	// XXX: no way to get info from xtr pluginz?
	// Note, object size can not be set from here due to potential
	// inconsistencies
	if (cp->size) {
		o->size = cp->size (binfile);
	}
	// XXX this is expensive because is O(n^n)
	if (cp->binsym) {
		for (i = 0; i < R_BIN_SYM_LAST; i++) {
			o->binsym[i] = cp->binsym (binfile, i);
			if (o->binsym[i]) {
				o->binsym[i]->paddr += o->loadaddr;
			}
		}
	}
	if (cp->entries) {
		o->entries = cp->entries (binfile);
		REBASE_PADDR (o, o->entries, RBinAddr);
	}
	if (cp->fields) {
		o->fields = cp->fields (binfile);
		if (o->fields) {
			o->fields->free = r_bin_field_free;
			REBASE_PADDR (o, o->fields, RBinField);
		}
	}
	if (cp->imports) {
		r_list_free (o->imports);
		o->imports = cp->imports (binfile);
		if (o->imports) {
			o->imports->free = r_bin_import_free;
		}
	}
	if (cp->symbols) {
		o->symbols = cp->symbols (binfile); // 5s
		if (o->symbols) {
			o->symbols->free = r_bin_symbol_free;
			REBASE_PADDR (o, o->symbols, RBinSymbol);
			if (bin->filter) {
				r_bin_filter_symbols (binfile, o->symbols); // 5s
			}
		}
	}
	o->info = cp->info? cp->info (binfile): NULL;
	if (cp->libs) {
		o->libs = cp->libs (binfile);
	}
	if (cp->sections) {
		// XXX sections are populated by call to size
		if (!o->sections) {
			o->sections = cp->sections (binfile);
		}
		REBASE_PADDR (o, o->sections, RBinSection);
		if (bin->filter) {
			r_bin_filter_sections (binfile, o->sections);
		}
	}
	if (bin->filter_rules & (R_BIN_REQ_RELOCS | R_BIN_REQ_IMPORTS)) {
		if (cp->relocs) {
			RList *l = cp->relocs (binfile);
			if (l) {
				REBASE_PADDR (o, l, RBinReloc);
				o->relocs = list2rbtree (l);
				l->free = NULL;
				r_list_free (l);
			}
		}
	}
	if (bin->filter_rules & R_BIN_REQ_STRINGS) {
		if (cp->strings) {
			o->strings = cp->strings (binfile);
		} else {
			o->strings = r_bin_file_get_strings (binfile, minlen, 0, binfile->rawstr);
		}
		if (bin->debase64) {
			r_bin_object_filter_strings (o);
		}
		REBASE_PADDR (o, o->strings, RBinString);
	}
	if (bin->filter_rules & R_BIN_REQ_CLASSES) {
		if (cp->classes) {
			o->classes = cp->classes (binfile);
			isSwift = r_bin_lang_swift (binfile);
			if (isSwift) {
				o->classes = classes_from_symbols (binfile);
			}
		} else {
			o->classes = classes_from_symbols (binfile);
		}
		if (bin->filter) {
			filter_classes (binfile, o->classes);
		}
		// cache addr=class+method
		if (o->classes) {
			RList *klasses = o->classes;
			RListIter *iter, *iter2;
			RBinClass *klass;
			RBinSymbol *method;
			if (!o->addr2klassmethod) {
				// this is slow. must be optimized, but at least its cached
				o->addr2klassmethod = sdb_new0 ();
				r_list_foreach (klasses, iter, klass) {
					r_list_foreach (klass->methods, iter2, method) {
						char *km = sdb_fmt ("method.%s.%s", klass->name, method->name);
						char *at = sdb_fmt ("0x%08"PFMT64x, method->vaddr);
						sdb_set (o->addr2klassmethod, at, km, 0);
					}
				}
			}
		}
	}
	if (cp->lines) {
		o->lines = cp->lines (binfile);
	}
	if (cp->get_sdb) {
		Sdb* new_kv = cp->get_sdb (binfile);
		if (new_kv != o->kv) {
			sdb_free (o->kv);
		}
		o->kv = new_kv;
	}
	if (cp->mem)  {
		o->mem = cp->mem (binfile);
	}
	if (bin->filter_rules & (R_BIN_REQ_SYMBOLS | R_BIN_REQ_IMPORTS)) {
		if (isSwift) {
			o->lang = R_BIN_NM_SWIFT;
		} else {
			o->lang = r_bin_load_languages (binfile);
		}
	}
	binfile->o = old_o;
	return true;
}

R_IPI RBNode *r_bin_object_patch_relocs(RBin *bin, RBinObject *o) {
	r_return_val_if_fail (bin && o, NULL);

	static bool first = true;
	// r_bin_object_set_items set o->relocs but there we don't have access
	// to io so we need to be run from bin_relocs, free the previous reloc and get
	// the patched ones
	if (first && o->plugin && o->plugin->patch_relocs) {
		RList *tmp = o->plugin->patch_relocs (bin);
		first = false;
		if (!tmp) {
			return o->relocs;
		}
		r_rbtree_free (o->relocs, reloc_free);
		REBASE_PADDR (o, tmp, RBinReloc);
		o->relocs = list2rbtree (tmp);
		first = false;
	}
	return o->relocs;
}

R_IPI RBinObject *r_bin_object_get_cur(RBin *bin) {
	r_return_val_if_fail (bin && bin->cur, NULL);
	return bin->cur->o;
}

R_IPI RBinObject *r_bin_object_find_by_arch_bits(RBinFile *bf, const char *arch, int bits, const char *name) {
	r_return_val_if_fail (bf && arch && name, NULL);
	if (!bf->o) {
		return NULL;
	}
	RBinObject *obj = bf->o;
	RBinInfo *info = obj->info;
	if (info && info->arch && info->file &&
			(bits == info->bits) &&
			!strcmp (info->arch, arch) &&
			!strcmp (info->file, name)) {
		return obj;
	}
	return NULL;
}

R_IPI ut64 r_bin_object_get_baddr(RBinObject *o) {
	r_return_val_if_fail (o, UT64_MAX);
	return o->baddr + o->baddr_shift;
}

R_API bool r_bin_object_delete(RBin *bin, ut32 bf_id, ut32 bo_id) {
	RBinFile *bf = NULL;
	RBinObject *obj = NULL;
	bool res = false;

	r_return_val_if_fail (bin, false);

	if (bf_id == UT32_MAX) {
		bf = r_bin_file_find_by_object_id (bin, bo_id);
		obj = bf ? r_bin_file_object_find_by_id (bf, bo_id) : NULL;
	} else if (bo_id == UT32_MAX) {
		bf = r_bin_file_find_by_id (bin, bf_id);
		obj = bf ? bf->o : NULL;
	} else {
		bf = r_bin_file_find_by_id (bin, bf_id);
		obj = bf ? r_bin_file_object_find_by_id (bf, bo_id) : NULL;
	}
	if (bf && bin->cur == bf) {
		bin->cur = NULL;
	}

	if (bf) {
		bf->o = NULL;
	}
	if (bf && obj && !bf->o) {
		r_list_delete_data (bin->binfiles, bf);
	}
	return res;
}

R_IPI void r_bin_object_set_baddr(RBinObject *o, ut64 baddr) {
	r_return_if_fail (o);
	if (baddr != UT64_MAX) {
		o->baddr_shift = baddr - o->baddr;
	}
}

R_IPI void r_bin_object_filter_strings(RBinObject *bo) {
	r_return_if_fail (bo);

	RList *strings = bo->strings;
	RBinString *ptr;
	RListIter *iter;
	r_list_foreach (strings, iter, ptr) {
		char *dec = (char *)r_base64_decode_dyn (ptr->string, -1);
		if (dec) {
			char *s = ptr->string;
			for (;;) {
				char *dec2 = (char *)r_base64_decode_dyn (s, -1);
				if (!dec2) {
					break;
				}
				if (!r_str_is_printable (dec2)) {
					free (dec2);
					break;
				}
				free (dec);
				s = dec = dec2;
			}
			if (r_str_is_printable (dec) && strlen (dec) > 3) {
				free (ptr->string);
				ptr->string = dec;
				ptr->type = R_STRING_TYPE_BASE64;
			} else {
				free (dec);
			}
		}
	}
}

R_API RBinFile *r_bin_file_at(RBin *bin, ut64 at) {
	RListIter *it, *it2;
	RBinFile *bf;
	RBinSection *s;
	r_list_foreach (bin->binfiles, it, bf) {
		r_list_foreach (bf->o->sections, it2, s) {
			if (at >= s->vaddr  && at < (s->vaddr + s->vsize)) {
				return bf;
			}
		}
	}
	return NULL;
}
