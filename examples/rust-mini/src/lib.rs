pub mod inner;

pub trait Greeter {
    fn hello(&self) -> &'static str;
}

pub enum Locale {
    EnUS,
    PtBR,
}

pub struct EnUS;
pub struct PtBR;

impl Greeter for EnUS {
    fn hello(&self) -> &'static str {
        shout!("hi")
    }
}

impl Greeter for PtBR {
    fn hello(&self) -> &'static str {
        shout!("oi")
    }
}

#[macro_export]
macro_rules! shout {
    ($s:expr) => {
        concat!($s, "!")
    };
}

pub use shout;
