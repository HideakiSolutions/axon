use crate::{Greeter, EnUS, PtBR, Locale};

pub fn greet(locale: Locale) -> &'static str {
    match locale {
        Locale::EnUS => EnUS.hello(),
        Locale::PtBR => PtBR.hello(),
    }
}
