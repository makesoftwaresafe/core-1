/*
  Copyright 2022 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#ifndef CFENGINE_CHANGES_CHROOT_H
#define CFENGINE_CHANGES_CHROOT_H

#define CHROOT_CHANGES_LIST_FILE "/changed_files"
#define CHROOT_RENAMES_LIST_FILE "/renamed_files"
#define CHROOT_KEPT_LIST_FILE "/kept_files"
#define CHROOT_PKGS_OPS_FILE "/pkgs_ops"

void PrepareChangesChroot(const char *path);
bool RecordFileChangedInChroot(const char *path);
bool RecordFileRenamedInChroot(const char *old_name, const char *new_name);
bool RecordFileEvaluatedInChroot(const char *path);
bool RecordPkgOperationInChroot(const char *op, const char *name, const char *arch, const char *version);

#endif /* CFENGINE_CHANGES_CHROOT_H */
