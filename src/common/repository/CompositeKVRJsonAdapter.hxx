//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2025 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#ifndef COMPOSITE_KVR_JSON_ADAPTER_HXX
#define COMPOSITE_KVR_JSON_ADAPTER_HXX

#include "repository/CompositeKeyValueRepository.hxx"
#include "repository/KeyValueRepository.hxx"
#include "bspf.hxx"

class CompositeKVRJsonAdapter : public CompositeKeyValueRepository {
  public:

    explicit CompositeKVRJsonAdapter(KeyValueRepositoryAtomic& kvr);

    shared_ptr<KeyValueRepository> get(string_view key) override;

    bool has(string_view key) override;

    void remove(string_view key) override;

  private:

    KeyValueRepositoryAtomic& myKvr;
};

#endif // COMPOSITE_KVR_JSON_ADAPTER_HXX
