//	Greenplum Database
//	Copyright (C) 2016 Pivotal Software, Inc.

#ifndef GPOPT_CMetadataAccessorFactory_H
#define GPOPT_CMetadataAccessorFactory_H

#include "gpos/memory/IMemoryPool.h"
#include "gpos/common/CAutoP.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/minidump/CDXLMinidump.h"

namespace gpopt
{
	class CMetadataAccessorFactory
	{
		public:
			CMetadataAccessorFactory
				(
					IMemoryPool *mp,
					CDXLMinidump *pdxlmd,
					const CHAR *file_name
				);

			CMDAccessor *Pmda();

		private:
			CAutoP<CMDAccessor> m_apmda;
	};

}
#endif
