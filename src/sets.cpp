/*
 * sets.cpp
 *
 *  Created on: 2013-4-2
 *      Author: wqy
 */

#include "rddb.hpp"
#include <tr1/unordered_set>
#include <tr1/unordered_map>

namespace rddb
{
	static bool DecodeSetMetaData(ValueObject& v, SetMetaValue& meta)
	{
		if (v.type != RAW)
		{
			return false;
		}
		return BufferHelper::ReadVarUInt32(*(v.v.raw), meta.size)
				&& BufferHelper::ReadVarString(*(v.v.raw), meta.min)
				&& BufferHelper::ReadVarString(*(v.v.raw), meta.max);
	}
	static void EncodeSetMetaData(ValueObject& v, SetMetaValue& meta)
	{
		v.type = RAW;
		if (v.v.raw == NULL)
		{
			v.v.raw = new Buffer(16);
		}
		BufferHelper::WriteVarUInt32(*(v.v.raw), meta.size);
		BufferHelper::WriteVarString(*(v.v.raw), meta.min);
		BufferHelper::WriteVarString(*(v.v.raw), meta.max);
	}

	int RDDB::GetSetMetaValue(DBID db, const Slice& key, SetMetaValue& meta)
	{
		KeyObject k(key, SET_META);
		ValueObject v;
		if (0 == GetValue(db, k, v))
		{
			if (!DecodeSetMetaData(v, meta))
			{
				return ERR_INVALID_TYPE;
			}
			return 0;
		}
		return ERR_NOT_EXIST;
	}
	void RDDB::SetSetMetaValue(DBID db, const Slice& key, SetMetaValue& meta)
	{
		KeyObject k(key, SET_META);
		ValueObject v;
		EncodeSetMetaData(v, meta);
		SetValue(db, k, v);
	}

	int RDDB::SAdd(DBID db, const Slice& key, const Slice& value)
	{
		KeyObject k(key, SET_META);
		SetMetaValue meta;
		GetSetMetaValue(db, key, meta);
		Slice min(meta.min);
		Slice max(meta.max);
		SetKeyObject sk(key, value);
		ValueObject sv;
		if (0 != GetValue(db, sk, sv))
		{
			meta.size++;
			if (min.size() == 0 || min.compare(value) > 0)
			{
				meta.min.assign(value.data(), value.size());
			}
			if (max.size() == 0 || max.compare(value) < 0)
			{
				meta.max.assign(value.data(), value.size());
			}
			sv.type = EMPTY;
			BatchWriteGuard guard(GetDB(db));
			SetValue(db, sk, sv);
			SetSetMetaValue(db, key, meta);
			return 1;
		}
		return 0;
	}

	int RDDB::SCard(DBID db, const Slice& key)
	{
		KeyObject k(key, SET_META);
		ValueObject v;
		SetMetaValue meta;
		if (0 == GetValue(db, k, v))
		{
			if (!DecodeSetMetaData(v, meta))
			{
				return ERR_INVALID_TYPE;
			}
			return meta.size;
		}
		return ERR_NOT_EXIST;
	}

	int RDDB::SIsMember(DBID db, const Slice& key, const Slice& value)
	{
		SetKeyObject sk(key, value);
		ValueObject sv;
		if (0 != GetValue(db, sk, sv))
		{
			return ERR_NOT_EXIST;
		}
		return 1;
	}

	int RDDB::SRem(DBID db, const Slice& key, const Slice& value)
	{
		KeyObject k(key, SET_META);
		ValueObject v;
		SetMetaValue meta;
		if (0 == GetValue(db, k, v))
		{
			if (!DecodeSetMetaData(v, meta))
			{
				return ERR_INVALID_TYPE;
			}
		}
		SetKeyObject sk(key, value);
		ValueObject sv;
		if (0 != GetValue(db, sk, sv))
		{
			meta.size--;
			sv.type = EMPTY;
			BatchWriteGuard guard(GetDB(db));
			DelValue(db, sk);
			EncodeSetMetaData(v, meta);
			return SetValue(db, k, v) == 0 ? 1 : -1;
		}
		return 0;
	}

	int RDDB::SMembers(DBID db, const Slice& key, StringArray& values)
	{
		Slice empty;
		SetKeyObject sk(key, empty);
		struct SMembersWalk: public WalkHandler
		{
				StringArray& z_values;
				int OnKeyValue(KeyObject* k, ValueObject* v)
				{
					SetKeyObject* sek = (SetKeyObject*) k;
					std::string tmp(sek->value.data(), sek->value.size());
					z_values.push_back(tmp);
					return 0;
				}
				SMembersWalk(StringArray& vs) :
						z_values(vs)
				{
				}
		} walk(values);
		Walk(db, sk, false, &walk);
		return 0;
	}

	int RDDB::SClear(DBID db, const Slice& key)
	{
		Slice empty;
		SetKeyObject sk(key, empty);
		struct SClearWalk: public WalkHandler
		{
				RDDB* z_db;
				DBID z_dbid;
				int OnKeyValue(KeyObject* k, ValueObject* v)
				{
					SetKeyObject* sek = (SetKeyObject*) k;
					z_db->DelValue(z_dbid, *sek);
					return 0;
				}
				SClearWalk(RDDB* db, DBID dbid) :
						z_db(db), z_dbid(dbid)
				{
				}
		} walk(this, db);
		BatchWriteGuard guard(GetDB(db));
		Walk(db, sk, false, &walk);
		KeyObject k(key, SET_META);
		DelValue(db, k);
		return 0;
	}

	int RDDB::SDiff(DBID db, SliceArray& keys, StringArray& values)
	{
		if (keys.size() < 2)
		{
			return ERR_INVALID_ARGS;
		}
		typedef std::tr1::unordered_set<std::string> ValueSet;
		typedef std::tr1::unordered_map<std::string, ValueSet> KeyValueSet;
		StringArray vs;
		KeyValueSet kvs;
		SMembers(db, keys.front(), vs);
		for (int i = 1; i < keys.size(); i++)
		{
			Slice k = keys.at(i);
			std::string tmp(k.data(), k.size());
			StringArray vss;
			SMembers(db, k, vss);
			StringArray::iterator vit = vss.begin();
			while (vit != vss.end())
			{
				kvs[tmp].insert(*vit);
				vit++;
			}
		}
		if (vs.size() > 0)
		{
			StringArray::iterator it = vs.begin();
			while (it < vs.begin())
			{
				std::string& v = *it;
				bool found = false;
				for (int i = 1; i < keys.size(); i++)
				{
					Slice k = keys.at(i);
					std::string tmp(k.data(), k.size());
					if (kvs[tmp].find(v) != kvs[tmp].end())
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					values.push_back(v);
				}
				it++;
			}
		}
		return 0;
	}

	int RDDB::SDiffStore(DBID db, const Slice& dst, SliceArray& keys)
	{
		if (keys.size() < 2)
		{
			return ERR_INVALID_ARGS;
		}
		StringArray vs;
		if (0 == SDiff(db, keys, vs) && vs.size() > 0)
		{
			SClear(db, dst);
			BatchWriteGuard guard(GetDB(db));
			KeyObject k(dst, SET_META);
			ValueObject smv;
			SetMetaValue meta;
			StringArray::iterator it = vs.begin();
			while (it < vs.begin())
			{
				std::string& v = *it;
				Slice sv(v.c_str(), v.size());
				SetKeyObject sk(dst, sv);
				ValueObject empty;
				empty.type = EMPTY;
				SetValue(db, sk, empty);
				meta.size++;
				it++;
			}
			EncodeSetMetaData(smv, meta);
			return SetValue(db, k, smv) == 0 ? meta.size : -1;
		}
		return 0;

	}

	int RDDB::SInter(DBID db, SliceArray& keys, StringArray& values)
	{
		if (keys.size() < 2)
		{
			return ERR_INVALID_ARGS;
		}
		uint32_t min_size = 0;
		typedef std::vector<SetMetaValue> SetMetaValueArray;
		SetMetaValueArray metas;
		uint32_t min_idx = 0;
		uint32_t idx = 0;
		std::string min;
		std::string max;
		SliceArray::iterator kit = keys.begin();
		while (kit != keys.end())
		{
			SetMetaValue meta;
			if (0 == GetSetMetaValue(db, *kit, meta))
			{
				if (min_size == 0 || min_size > meta.size)
				{
					min_size = meta.size;
					min_idx = idx;
				}
			}
			if (min.size() == 0 || min.compare(meta.min) < 0)
			{
				min = meta.min;
			}
			if (max.size() == 0 || max.compare(meta.max) > 0)
			{
				max = meta.max;
			}
			metas.push_back(meta);
			kit++;
			idx++;
		}

		typedef std::tr1::unordered_set<std::string> ValueSet;
		struct SInterWalk: public WalkHandler
		{
				ValueSet& z_cmp;
				ValueSet& z_result;
				const std::string& s_min;
				const std::string& s_max;
				const std::string* current_min;
				const std::string* current_max;
				SInterWalk(ValueSet& cmp, ValueSet& result,
						const std::string& min, const std::string& max) :
						z_cmp(cmp), z_result(result), s_min(min), s_max(max), current_min(
								NULL), current_max(NULL)
				{
				}
				int OnKeyValue(KeyObject* k, ValueObject* value)
				{
					SetKeyObject* sk = (SetKeyObject*) k;
					std::string vstr(sk->value.data(), sk->value.size());
					if (vstr.compare(s_min) < 0)
					{
						return 0;
					}
					if (vstr.compare(s_max) > 0)
					{
						return -1;
					}
					if ((&z_cmp != &z_result) && z_cmp.count(vstr) == 0)
					{
						return 0;
					}
					std::pair<ValueSet::iterator, bool> iter = z_result.insert(
							vstr);
					if (NULL == current_min)
					{

						current_min = &(*(iter.first));
					}
					current_max = &(*(iter.first));
					return 0;
				}
		};
		ValueSet cmp1, cmp2;
		Slice min_start(min);
		SetKeyObject cmp_start(keys[min_idx], min_start);
		SInterWalk walk(cmp1, cmp1, min, max);
		Walk(db, cmp_start, false, &walk);
		ValueSet* cmp = &cmp1;
		ValueSet* result = &cmp2;
		for (uint32_t i = 0; i < keys.size(); i++)
		{
			if (i != min_idx)
			{
				Slice min_start(min);
				SetKeyObject tmp(keys.at(i), min_start);
				SInterWalk walk(*cmp, *result, min, max);
				Walk(db, tmp, false, &walk);
				cmp->clear();
				ValueSet* old = cmp;
				cmp = result;
				result = old;
				if (min.compare(*walk.current_min) < 0)
				{
					min = *walk.current_min;
				}
				if (max.compare(*walk.current_max) > 0)
				{
					max = *walk.current_max;
				}
			}
		}

		if (cmp->size() > 0)
		{
			ValueSet::iterator it = cmp->begin();
			while (it != cmp->end())
			{
				const std::string& v = *it;
				values.push_back(v);
				it++;
			}
		}
		return 0;
	}

	int RDDB::SInterStore(DBID db, const Slice& dst, SliceArray& keys)
	{
		if (keys.size() < 2)
		{
			return ERR_INVALID_ARGS;
		}
		StringArray vs;
		if (0 == SInter(db, keys, vs) && vs.size() > 0)
		{
			SClear(db, dst);
			BatchWriteGuard guard(GetDB(db));
			KeyObject k(dst, SET_META);
			ValueObject smv;
			SetMetaValue meta;
			StringArray::iterator it = vs.begin();
			while (it < vs.begin())
			{
				std::string& v = *it;
				Slice sv(v.c_str(), v.size());
				SetKeyObject sk(dst, sv);
				ValueObject empty;
				empty.type = EMPTY;
				SetValue(db, sk, empty);
				meta.size++;
				it++;
			}
			EncodeSetMetaData(smv, meta);
			return SetValue(db, k, smv) == 0 ? meta.size : -1;
		}
		return 0;
	}

	int RDDB::SMove(DBID db, const Slice& src, const Slice& dst,
			const Slice& value)
	{
		SetKeyObject sk(src, value);
		ValueObject sv;
		if (0 != GetValue(db, sk, sv))
		{
			return 0;
		}
		sk.key = dst;
		SetValue(db, sk, sv);
		return 1;
	}

	int RDDB::SPop(DBID db, const Slice& key, std::string& value)
	{
		Slice empty;
		SetKeyObject sk(key, empty);
		Iterator* iter = FindValue(db, sk);
		while (iter != NULL && iter->Valid())
		{
			Slice tmpkey = iter->Key();
			KeyObject* kk = decode_key(tmpkey);
			if (NULL == kk || kk->type != SET_ELEMENT
					|| kk->key.compare(key) != 0)
			{
				DELETE(kk);
				break;
			}
			SetKeyObject* sek = (SetKeyObject*) kk;
			value.append(sek->value.data(), sek->value.size());
			DelValue(db, *sek);
			DELETE(kk);
			return 0;
		}
		return -1;
	}

	int RDDB::SRandMember(DBID db, const Slice& key, StringArray& values,
			int count)
	{
		Slice empty;
		SetKeyObject sk(key, empty);
		Iterator* iter = FindValue(db, sk);
		int total = count;
		if (count < 0)
		{
			total = 0 - count;
		}
		int cursor = 0;
		while (iter != NULL && iter->Valid())
		{
			Slice tmpkey = iter->Key();
			KeyObject* kk = decode_key(tmpkey);
			if (NULL == kk || kk->type != SET_ELEMENT
					|| kk->key.compare(key) != 0)
			{
				DELETE(kk);
				break;
			}
			SetKeyObject* sek = (SetKeyObject*) kk;
			values.push_back(std::string(sek->value.data(), sek->value.size()));
			DELETE(kk);
			iter->Next();
			cursor++;
			if (cursor == total)
			{
				break;
			}
		}
		DELETE(iter);
		while (count < 0 && values.size() < total)
		{
			values.push_back(values.front());
		}
		return 0;
	}

	int RDDB::SUnion(DBID db, SliceArray& keys, StringArray& values)
	{
		typedef std::tr1::unordered_set<std::string> ValueSet;
		ValueSet kvs;
		for (int i = 0; i < keys.size(); i++)
		{
			Slice k = keys.at(i);
			std::string tmp(k.data(), k.size());
			StringArray vss;
			SMembers(db, k, vss);
			StringArray::iterator vit = vss.begin();
			while (vit != vss.end())
			{
				kvs.insert(*vit);
				vit++;
			}
		}
		ValueSet::iterator vit = kvs.begin();
		while (vit != kvs.end())
		{
			values.push_back(*vit);
			vit++;
		}
		return 0;
	}

	int RDDB::SUnionStore(DBID db, const Slice& dst, SliceArray& keys)
	{
		StringArray ss;
		if (0 == SUnion(db, keys, ss) && ss.size() > 0)
		{
			SClear(db, dst);
			BatchWriteGuard guard(GetDB(db));
			KeyObject k(dst, SET_META);
			ValueObject smv;
			SetMetaValue meta;
			StringArray::iterator it = ss.begin();
			while (it < ss.begin())
			{
				std::string& v = *it;
				Slice sv(v.c_str(), v.size());
				SetKeyObject sk(dst, sv);
				ValueObject empty;
				empty.type = EMPTY;
				SetValue(db, sk, empty);
				meta.size++;
				it++;
			}
			EncodeSetMetaData(smv, meta);
			return SetValue(db, k, smv) == 0 ? meta.size : -1;
		}
		return 0;
	}

}

