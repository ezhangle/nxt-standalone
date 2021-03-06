// Copyright 2017 The NXT Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include "nxt/nxtcpp.h"

class Object : public nxt::ObjectBase<Object, int*> {
    public:
        using ObjectBase::ObjectBase;
        using ObjectBase::operator=;

        static void NxtReference(int* handle) {
            ASSERT_LT(0, *handle);
            *handle += 1;
        }
        static void NxtRelease(int* handle) {
            ASSERT_LT(0, *handle);
            *handle -= 1;
        }
};

// Test that creating an C++ object from a C object takes a ref.
// Also test that the C++ object destructor removes a ref.
TEST(ObjectBase, CTypeConstructor) {
    int refcount = 1;
    {
        Object obj(&refcount);
        ASSERT_EQ(2, refcount);
    }
    ASSERT_EQ(1, refcount);
}

// Test consuming a C object into a C++ object doesn't take a ref.
TEST(ObjectBase, AcquireConstruction) {
    int refcount = 1;
    {
        Object object = Object::Acquire(&refcount);
        ASSERT_EQ(1, refcount);
    }
    ASSERT_EQ(0, refcount);
}

// Test that cloning takes a new ref. Also test .Get().
TEST(ObjectBase, Clone) {
    int refcount = 1;
    {
        Object obj1(&refcount);
        Object obj2 = obj1.Clone();

        ASSERT_EQ(3, refcount);
        ASSERT_EQ(&refcount, obj1.Get());
        ASSERT_EQ(&refcount, obj2.Get());
    }
    ASSERT_EQ(1, refcount);
}

// Test that Release consumes the C++ object into a C object and doesn't release
TEST(ObjectBase, Release) {
    int refcount = 1;
    {
        Object obj(&refcount);
        ASSERT_EQ(2, refcount);

        ASSERT_EQ(&refcount, obj.Release());
        ASSERT_EQ(nullptr, obj.Get());
        ASSERT_EQ(2, refcount);
    }
    ASSERT_EQ(2, refcount);
}

// Test using C++ objects in conditions
TEST(ObjectBase, OperatorBool) {
    int refcount = 1;
    Object trueObj(&refcount);
    Object falseObj;

    if (falseObj || !trueObj) {
        ASSERT_TRUE(false);
    }
}

// Test the move constructor of C++ objects
TEST(ObjectBase, MoveConstructor) {
    int refcount = 1;
    Object source(&refcount);
    Object destination(std::move(source));

    ASSERT_EQ(source.Get(), nullptr);
    ASSERT_EQ(destination.Get(), &refcount);
    ASSERT_EQ(2, refcount);

    destination = Object();
    ASSERT_EQ(refcount, 1);
}

// Test the move assignment of C++ objects
TEST(ObjectBase, MoveAssignment) {
    int refcount = 1;
    Object source(&refcount);

    Object destination;
    destination = std::move(source);

    ASSERT_EQ(source.Get(), nullptr);
    ASSERT_EQ(destination.Get(), &refcount);
    ASSERT_EQ(2, refcount);

    destination = Object();
    ASSERT_EQ(refcount, 1);
}

